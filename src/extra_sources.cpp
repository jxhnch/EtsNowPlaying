#include "extra_sources.h"

#include "http.h"
#include "log.h"
#include "settings.h"
#include "text_util.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ep {
namespace {

// ---- a small JSON reader (only what a "where is the song" path needs) ----------------------------------------------

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string text;  // String, or a number as written
    std::vector<std::string> keys;  // Object
    std::vector<Json> values;       // Object (parallel to keys) or Array
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool Parse(Json& out) {
        Skip();
        return Value(out, 0);
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    void Skip() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
    }
    bool Literal(const char* word) {
        const size_t n = strlen(word);
        if (s_.compare(p_, n, word) != 0) return false;
        p_ += n;
        return true;
    }
    bool Str(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        out.clear();
        std::wstring pending;  // UTF-16 units of \uXXXX escapes (surrogate pairs end up as one character)
        auto flush = [&] {
            if (!pending.empty()) out += ToUtf8(pending);
            pending.clear();
        };
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == '"') {
                flush();
                ++p_;
                return true;
            }
            if (c == '\\' && p_ + 1 < s_.size()) {
                const char e = s_[p_ + 1];
                if (e == 'u') {
                    if (p_ + 6 > s_.size()) return false;
                    pending.push_back(static_cast<wchar_t>(strtoul(s_.substr(p_ + 2, 4).c_str(), nullptr, 16)));
                    p_ += 6;
                    continue;
                }
                flush();
                out.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e);
                p_ += 2;
                continue;
            }
            flush();
            out.push_back(c);
            ++p_;
        }
        return false;
    }
    bool Value(Json& out, int depth) {
        if (depth > 24 || p_ >= s_.size()) return false;
        const char c = s_[p_];
        if (c == '{') {
            ++p_;
            out.type = Json::Object;
            Skip();
            if (p_ < s_.size() && s_[p_] == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                Skip();
                std::string key;
                if (!Str(key)) return false;
                Skip();
                if (p_ >= s_.size() || s_[p_] != ':') return false;
                ++p_;
                Skip();
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.keys.push_back(std::move(key));
                out.values.push_back(std::move(v));
                Skip();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            out.type = Json::Array;
            Skip();
            if (p_ < s_.size() && s_[p_] == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                Skip();
                Json v;
                if (!Value(v, depth + 1)) return false;
                out.values.push_back(std::move(v));
                Skip();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (s_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type = Json::String;
            return Str(out.text);
        }
        if (Literal("true") || Literal("false")) {
            out.type = Json::Bool;
            return true;
        }
        if (Literal("null")) {
            out.type = Json::Null;
            return true;
        }
        const size_t start = p_;
        while (p_ < s_.size() && strchr("+-0123456789.eE", s_[p_])) ++p_;
        if (p_ == start) return false;
        out.type = Json::Number;
        out.text = s_.substr(start, p_ - start);
        return true;
    }
};

// "response.activeSession.rawTitle", array items by number ("items.0.title").
const Json* Find(const Json& root, const std::string& path) {
    const Json* cur = &root;
    size_t i = 0;
    for (;;) {
        const size_t dot = path.find('.', i);
        const std::string part = path.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty() || !cur) return nullptr;
        if (cur->type == Json::Object) {
            const Json* next = nullptr;
            for (size_t k = 0; k < cur->keys.size(); ++k) {
                if (cur->keys[k] == part) {
                    next = &cur->values[k];
                    break;
                }
            }
            cur = next;
        } else if (cur->type == Json::Array && part.find_first_not_of("0123456789") == std::string::npos) {
            const size_t idx = static_cast<size_t>(atoi(part.c_str()));
            cur = idx < cur->values.size() ? &cur->values[idx] : nullptr;
        } else {
            return nullptr;
        }
        if (dot == std::string::npos) return cur;
        i = dot + 1;
    }
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Several alternatives separated by '|': the first path that leads to a non-empty text wins.
std::string FirstText(const Json& root, const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        const Json* v = Find(root, path);
        if (v && (v->type == Json::String || v->type == Json::Number)) {
            std::string t = Trim(v->text);
            if (!t.empty()) return t;
        }
    }
    return {};
}

// ---- the sources file ----------------------------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::vector<std::string> match;  // lower case; any of them found in the station's host / name / icy-name
    std::wstring url;
    std::vector<std::string> titlePaths, artistPaths;
    int intervalSec = 10;
};

struct Runtime {
    ULONGLONG lastFetch = 0;
    std::string value;  // the song, empty = nothing (fall back to the stream's title)
    ULONGLONG valueTick = 0;
    uint64_t serial = 0;
    int failures = 0;
};

std::vector<Entry> g_entries;  // loaded once before the thread starts, never changed afterwards
std::vector<Runtime> g_rt;
std::mutex g_mx;
std::thread g_thread;
std::atomic<bool> g_stop{false};

const char kDefaultFile[] =
    "; EtsNowPlaying - OPTIONAL extra \"now playing\" sources\r\n"
    ";\r\n"
    "; You do not need to change anything here, and you can delete this file's entries at any time.\r\n"
    ";\r\n"
    "; Some radio stations only send the name of a show in their stream, not the song that is playing. If such a\r\n"
    "; station also offers the current song on a web address (JSON), it can be listed here. The plugin then asks that\r\n"
    "; address every few seconds, but ONLY while that station is playing. If the address gives no song (or cannot be\r\n"
    "; reached), the title from the stream is shown as usual.\r\n"
    ";\r\n"
    "; enabled   1 = use this entry, 0 = off. (EtsNowPlaying.ini: [radio] extra_sources=0 switches all entries off.)\r\n"
    "; match     words that identify the station: part of the stream's host or of its name, separated by |\r\n"
    "; url       the web address that returns JSON\r\n"
    "; title     where the song is inside that JSON, e.g. response.activeSession.rawTitle (alternatives separated\r\n"
    ";           by | - the first one that is not empty wins)\r\n"
    "; artist    optional, the same for the artist (shown as \"artist - title\")\r\n"
    "; interval  seconds between requests, at least 5 (default 10)\r\n"
    "\r\n"
    "[Real Hardstyle]\r\n"
    "; During its shows the stream of Real Hardstyle only carries the show name; the song is on their website.\r\n"
    "enabled=1\r\n"
    "match=realhardstyle|real hardstyle\r\n"
    "url=https://api.realhardstyle.com/get/nowplaying\r\n"
    "title=response.activeSession.rawTitle\r\n"
    "interval=10\r\n";

std::wstring SourcesPath() { return PluginDir() + L"\\EtsNowPlaying.sources.ini"; }

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t b = 0;
    for (;;) {
        const size_t e = s.find(sep, b);
        std::string part = Trim(s.substr(b, e == std::string::npos ? std::string::npos : e - b));
        if (!part.empty()) out.push_back(part);
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

std::string ReadKey(const std::wstring& ini, const std::wstring& section, const wchar_t* key) {
    wchar_t buf[2048] = {};
    GetPrivateProfileStringW(section.c_str(), key, L"", buf, 2048, ini.c_str());
    return Trim(ToUtf8(buf));
}

void LoadEntries() {
    const std::wstring ini = SourcesPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(ini.c_str(), L"wb");
        if (f) {
            fwrite(kDefaultFile, 1, sizeof(kDefaultFile) - 1, f);
            fclose(f);
            Logf("SOURCES wrote the optional sources file %ls", ini.c_str());
        }
    }
    std::vector<wchar_t> names(16384);
    const DWORD n = GetPrivateProfileSectionNamesW(names.data(), static_cast<DWORD>(names.size()), ini.c_str());
    int disabled = 0;
    for (const wchar_t* p = names.data(); n > 0 && *p; p += wcslen(p) + 1) {
        const std::wstring section = p;
        Entry e;
        e.name = ToUtf8(section);
        if (GetPrivateProfileIntW(section.c_str(), L"enabled", 1, ini.c_str()) == 0) {
            ++disabled;
            continue;
        }
        for (std::string m : Split(ReadKey(ini, section, L"match"), '|')) {
            for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            e.match.push_back(m);
        }
        const std::string url = ReadKey(ini, section, L"url");
        e.titlePaths = Split(ReadKey(ini, section, L"title"), '|');
        e.artistPaths = Split(ReadKey(ini, section, L"artist"), '|');
        e.intervalSec = std::max(5, static_cast<int>(GetPrivateProfileIntW(section.c_str(), L"interval", 10, ini.c_str())));
        const bool httpUrl = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
        if (e.match.empty() || e.titlePaths.empty() || !httpUrl) {
            Logf("SOURCES entry [%s] ignored: it needs match, title and an http(s) url", e.name.c_str());
            continue;
        }
        e.url = ToWide(url);
        g_entries.push_back(std::move(e));
    }
    g_rt.assign(g_entries.size(), Runtime{});
    Logf("SOURCES %zu entr%s active, %d switched off", g_entries.size(), g_entries.size() == 1 ? "y" : "ies", disabled);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

int Match(const RadioSnapshot& s) {
    if (!s.active) return -1;
    const std::string hay[] = {Lower(s.host), Lower(s.stationName), Lower(s.icyName)};
    for (size_t i = 0; i < g_entries.size(); ++i) {
        for (const std::string& m : g_entries[i].match) {
            for (const std::string& h : hay) {
                if (!h.empty() && h.find(m) != std::string::npos) return static_cast<int>(i);
            }
        }
    }
    return -1;
}

void Fetch(int idx) {
    const Entry& e = g_entries[static_cast<size_t>(idx)];
    std::vector<uint8_t> body;
    std::string value;
    bool ok = HttpGet(e.url, body, 4000, 256 * 1024);
    if (ok) {
        const std::string text(body.begin(), body.end());
        Json root;
        JsonParser parser(text);
        if (parser.Parse(root)) {
            const std::string title = FirstText(root, e.titlePaths);
            const std::string artist = FirstText(root, e.artistPaths);
            if (!title.empty()) value = artist.empty() ? title : artist + " - " + title;
        } else {
            ok = false;
        }
    }
    std::lock_guard<std::mutex> lock(g_mx);
    Runtime& rt = g_rt[static_cast<size_t>(idx)];
    if (!ok) {
        if (rt.failures++ < 3 || rt.failures % 30 == 0) {
            Logf("SOURCES [%s] request failed (%d in a row), the stream's own title is used", e.name.c_str(), rt.failures);
        }
        return;
    }
    rt.failures = 0;
    rt.valueTick = GetTickCount64();
    if (value != rt.value) {
        rt.value = value;
        ++rt.serial;
        Logf("SOURCES [%s] song: %s", e.name.c_str(), value.empty() ? "(none, using the stream's title)" : value.c_str());
    }
}

void Run() {
    int lastIdx = -1;
    while (!g_stop) {
        const int idx = Match(RadioGetSnapshot());
        if (idx != lastIdx) {  // station changed: forget the old song, ask right away
            std::lock_guard<std::mutex> lock(g_mx);
            if (lastIdx >= 0) g_rt[static_cast<size_t>(lastIdx)].value.clear();
            if (idx >= 0) g_rt[static_cast<size_t>(idx)].lastFetch = 0;
            lastIdx = idx;
        }
        if (idx >= 0) {
            bool due = false;
            {
                std::lock_guard<std::mutex> lock(g_mx);
                Runtime& rt = g_rt[static_cast<size_t>(idx)];
                const ULONGLONG now = GetTickCount64();
                due = now - rt.lastFetch >= static_cast<ULONGLONG>(g_entries[static_cast<size_t>(idx)].intervalSec) * 1000;
                if (due) rt.lastFetch = now;
            }
            if (due) Fetch(idx);
        }
        for (int i = 0; i < 10 && !g_stop; ++i) Sleep(100);
    }
}

}  // namespace

void ExtraSourcesStart() {
    if (!GetSettings().extraSources) {
        Logf("SOURCES switched off ([radio] extra_sources=0)");
        return;
    }
    LoadEntries();
    if (g_entries.empty()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void ExtraSourcesStop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

void ExtraSourcesApply(RadioSnapshot& s) {
    if (g_entries.empty()) return;
    const int idx = Match(s);
    if (idx < 0) return;
    std::lock_guard<std::mutex> lock(g_mx);
    const Runtime& rt = g_rt[static_cast<size_t>(idx)];
    const ULONGLONG maxAge = std::max<ULONGLONG>(30000, static_cast<ULONGLONG>(g_entries[static_cast<size_t>(idx)].intervalSec) * 3000);
    if (rt.value.empty() || GetTickCount64() - rt.valueTick > maxAge) return;
    s.title = rt.value;
    s.titleSerial += rt.serial << 40;  // a new song counts as a new title for the screen's fades and cover lookup
}

}  // namespace ep
