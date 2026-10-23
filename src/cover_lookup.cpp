#include "cover_lookup.h"

#include "http.h"
#include "log.h"
#include "settings.h"
#include "text_util.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace ep {
namespace {

struct Entry {
    CoverState state = CoverState::Pending;
    std::shared_ptr<const std::vector<uint8_t>> image;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<std::string, Entry> g_cache;
std::vector<std::string> g_order;  // insertion order, for trimming the cache
std::string g_pending;             // newest title waiting for the worker
std::thread g_thread;
std::atomic<bool> g_stop{false};

// Lower case, letters and digits only: "Don't Stop (Radio Edit)" -> "dontstopradioedit"
std::wstring Normalize(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (iswalnum(c)) out.push_back(static_cast<wchar_t>(towlower(c)));
    }
    return out;
}

// Drops "(...)" and "[...]" parts: "Song (feat. X) [Remastered]" -> "Song"
std::wstring StripBrackets(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'[') ++depth;
        else if ((c == L')' || c == L']') && depth > 0) --depth;
        else if (depth == 0) out.push_back(c);
    }
    return out;
}

bool Similar(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos;
}

std::string UrlEncode(const std::string& utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// Reads the JSON string value following `"key":"` inside `obj`. Handles the usual escapes.
bool JsonString(const std::string& obj, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t p = obj.find(needle);
    if (p == std::string::npos) return false;
    p += needle.size();
    out.clear();
    while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\' && p + 1 < obj.size()) {
            char e = obj[p + 1];
            if (e == 'u' && p + 5 < obj.size()) {
                unsigned code = static_cast<unsigned>(strtoul(obj.substr(p + 2, 4).c_str(), nullptr, 16));
                out += ToUtf8(std::wstring(1, static_cast<wchar_t>(code)));
                p += 6;
                continue;
            }
            out.push_back(e == 'n' ? '\n' : e);
            p += 2;
            continue;
        }
        out.push_back(obj[p++]);
    }
    return true;
}

bool Lookup(const std::string& streamTitle, std::shared_ptr<const std::vector<uint8_t>>& image) {
    std::wstring artist, track;
    if (!SplitArtistTitle(streamTitle, artist, track)) return false;  // no "Artist - Song": nothing to search for
    track = StripBrackets(track);
    if (Normalize(artist).empty() || Normalize(track).empty()) return false;

    std::string term = UrlEncode(ToUtf8(artist + L" " + track));
    std::wstring url = L"https://itunes.apple.com/search?media=music&entity=song&limit=8&term=" +
                       std::wstring(term.begin(), term.end());
    std::vector<uint8_t> body;
    if (!HttpGet(url, body, 6000, 1024 * 1024)) {
        Logf("COVER search failed for '%s'", streamTitle.c_str());
        return false;
    }
    std::string json(body.begin(), body.end());

    const std::wstring wantArtist = Normalize(artist), wantTrack = Normalize(StripBrackets(track));
    size_t pos = 0;
    while ((pos = json.find("{\"wrapperType\"", pos)) != std::string::npos) {
        size_t next = json.find("{\"wrapperType\"", pos + 1);
        std::string obj = json.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? json.size() : next;

        std::string gotArtist, gotTrack, art;
        if (!JsonString(obj, "artistName", gotArtist) || !JsonString(obj, "trackName", gotTrack) ||
            !JsonString(obj, "artworkUrl100", art)) {
            continue;
        }
        if (!Similar(Normalize(ToWide(gotArtist)), wantArtist) || !Similar(Normalize(StripBrackets(ToWide(gotTrack))), wantTrack)) {
            continue;
        }
        size_t size = art.find("100x100");
        if (size != std::string::npos) art.replace(size, 7, "600x600");
        std::vector<uint8_t> img;
        if (!HttpGet(ToWide(art), img, 8000, 4u * 1024 * 1024) || img.empty()) {
            Logf("COVER download failed: %s", art.c_str());
            return false;
        }
        image = std::make_shared<std::vector<uint8_t>>(std::move(img));
        Logf("COVER found for '%s' -> '%s' by '%s' (%zu bytes)", streamTitle.c_str(), gotTrack.c_str(), gotArtist.c_str(),
             image->size());
        return true;
    }
    Logf("COVER no match for '%s'", streamTitle.c_str());
    return false;
}

void Run() {
    while (!g_stop) {
        std::string title;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_stop || !g_pending.empty(); });
            if (g_stop) return;
            title = std::move(g_pending);
            g_pending.clear();
        }
        std::shared_ptr<const std::vector<uint8_t>> image;
        bool ok = false;
        try {
            ok = Lookup(title, image);
        } catch (...) {
            Logf("COVER lookup raised an exception");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        Entry& e = g_cache[title];
        e.state = ok ? CoverState::Found : CoverState::NotFound;
        e.image = image;
    }
}

}  // namespace

void CoverLookupStart() {
    g_stop = false;
    g_thread = std::thread(Run);
}

void CoverLookupStop() {
    g_stop = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void CoverLookupRequest(const std::string& streamTitle) {
    if (!GetSettings().coverLookup || streamTitle.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cache.count(streamTitle)) return;
    g_cache[streamTitle] = Entry{};
    g_order.push_back(streamTitle);
    if (g_order.size() > 64) {  // keep the cache small
        g_cache.erase(g_order.front());
        g_order.erase(g_order.begin());
    }
    g_pending = streamTitle;
    g_cv.notify_one();
}

CoverResult CoverLookupGet(const std::string& streamTitle) {
    CoverResult r;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(streamTitle);
    if (it == g_cache.end()) return r;
    r.state = it->second.state;
    r.image = it->second.image;
    return r;
}

}  // namespace ep
