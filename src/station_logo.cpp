#include "station_logo.h"

#include "http.h"
#include "log.h"
#include "settings.h"
#include "text_util.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace ep {
namespace {

struct Job {
    std::string key, name, country, url;
};

struct Entry {
    LogoState state = LogoState::Pending;
    std::shared_ptr<const std::vector<uint8_t>> image;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::map<std::string, Entry> g_cache;
std::deque<Job> g_queue;
std::thread g_thread;
std::atomic<bool> g_stop{false};

std::string KeyOf(const std::string& name, const std::string& url) { return name + "|" + url; }

std::wstring CachePath(const std::string& key, const wchar_t* ext) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    wchar_t name[40];
    swprintf(name, 40, L"%016llx", static_cast<unsigned long long>(h));
    return PluginDir() + L"\\logos\\" + name + ext;
}

bool ReadFile(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n > 0 && n < 4 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(n));
        ok = fread(out.data(), 1, out.size(), f) == out.size();
    }
    fclose(f);
    return ok;
}

void WriteFile(const std::wstring& path, const std::vector<uint8_t>& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

bool FileIsRecent(const std::wstring& path, int days) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER n, w;
    n.LowPart = now.dwLowDateTime;
    n.HighPart = now.dwHighDateTime;
    w.LowPart = a.ftLastWriteTime.dwLowDateTime;
    w.HighPart = a.ftLastWriteTime.dwHighDateTime;
    return n.QuadPart > w.QuadPart && (n.QuadPart - w.QuadPart) < static_cast<ULONGLONG>(days) * 864000000000ull;
}

const wchar_t* kServers[] = {L"https://de1.api.radio-browser.info", L"https://at1.api.radio-browser.info",
                             L"https://nl1.api.radio-browser.info"};

// Fetches `path` from the first radio-browser mirror that answers.
bool ApiGet(const std::wstring& path, std::string& json) {
    for (const wchar_t* server : kServers) {
        std::vector<uint8_t> body;
        if (HttpGet(std::wstring(server) + path, body, 6000, 2 * 1024 * 1024)) {
            json.assign(body.begin(), body.end());
            return true;
        }
    }
    return false;
}

std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }  // ASCII only (already URL-encoded)

// radio-browser returns an array of station objects, each starting with {"changeuuid":
struct Hit {
    std::string name, favicon;
};
std::vector<Hit> ParseStations(const std::string& json) {
    std::vector<Hit> hits;
    size_t pos = 0;
    while ((pos = json.find("{\"changeuuid\"", pos)) != std::string::npos) {
        size_t next = json.find("{\"changeuuid\"", pos + 1);
        std::string obj = json.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? json.size() : next;
        Hit h;
        JsonString(obj, "name", h.name);
        JsonString(obj, "favicon", h.favicon);
        if (!h.favicon.empty() && h.favicon.rfind("http", 0) == 0) hits.push_back(std::move(h));
    }
    return hits;
}

// The first bytes of a real image. Some "favicon" addresses of radio-browser lead to a web page (status 200, HTML).
bool LooksLikeImage(const std::vector<uint8_t>& b) {
    if (b.size() < 16) return false;
    const uint8_t* p = b.data();
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return true;
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return true;
    if (memcmp(p, "GIF8", 4) == 0) return true;
    if (p[0] == 'B' && p[1] == 'M') return true;
    if (memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0) return true;
    if (p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0) return true;  // icon
    return false;
}

void AddUnique(std::vector<std::string>& urls, const std::string& url) {
    if (!url.empty() && urls.size() < 8 && std::find(urls.begin(), urls.end(), url) == urls.end()) urls.push_back(url);
}

// Candidate logo addresses, best first: the entries with the very stream URL, then the name matches.
bool FindFaviconUrls(const Job& job, std::vector<std::string>& urls) {
    std::string json;
    // 1. The very stream URL from live_streams.sii.
    if (!job.url.empty() && ApiGet(L"/json/stations/byurl?url=" + W(UrlEncode(job.url)), json)) {
        for (const Hit& h : ParseStations(json)) AddUnique(urls, h.favicon);
    }
    // 2. By name (and country when the list has a real country code), exact name matches first.
    std::wstring path = L"/json/stations/search?hidebroken=true&order=votes&reverse=true&limit=15&name=" +
                        W(UrlEncode(job.name));
    if (job.country.size() == 2) path += L"&countrycode=" + W(UrlEncode(job.country));
    if (ApiGet(path, json)) {
        const std::wstring want = NormalizeKey(ToWide(job.name));
        const auto hits = ParseStations(json);
        for (const Hit& h : hits) {
            if (NormalizeKey(ToWide(h.name)) == want) AddUnique(urls, h.favicon);
        }
        for (const Hit& h : hits) {
            if (want.size() >= 3 && SimilarKeys(NormalizeKey(ToWide(h.name)), want)) AddUnique(urls, h.favicon);
        }
    }
    return !urls.empty();
}

// Own pictures: <plugin folder>\logos\custom\<station name>.png (or .jpg, .webp, .gif, .bmp, .ico). They win over everything else
// and need no network.
std::wstring SafeFileName(const std::string& name) {
    std::wstring w = ToWide(name);
    for (auto& c : w) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    }
    while (!w.empty() && (w.back() == L'.' || w.back() == L' ')) w.pop_back();
    return w;
}

bool ReadCustomLogo(const std::string& stationName, std::vector<uint8_t>& out) {
    const std::wstring base = SafeFileName(stationName);
    if (base.empty()) return false;
    for (const wchar_t* ext : {L".png", L".jpg", L".jpeg", L".webp", L".gif", L".bmp", L".ico"}) {
        std::vector<uint8_t> b;
        if (ReadFile(PluginDir() + L"\\logos\\custom\\" + base + ext, b) && LooksLikeImage(b)) {
            out = std::move(b);
            return true;
        }
    }
    return false;
}

void Process(const Job& job) {
    std::shared_ptr<const std::vector<uint8_t>> image;
    LogoState state = LogoState::NotFound;
    const std::wstring img = CachePath(job.key, L".img"), none = CachePath(job.key, L".none");

    std::vector<uint8_t> bytes;
    if (ReadCustomLogo(job.name, bytes)) {
        image = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
        state = LogoState::Found;
        Logf("LOGO '%s': own picture from logos\\custom (%zu bytes)", job.name.c_str(), image->size());
    } else {
        if (ReadFile(img, bytes) && !LooksLikeImage(bytes)) {  // an earlier version cached a web page as a logo
            DeleteFileW(img.c_str());
            bytes.clear();
        }
        if (!bytes.empty()) {
            image = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
            state = LogoState::Found;
        } else if (GetSettings().coverLookup && !FileIsRecent(none, 7)) {
            std::vector<std::string> urls;
            if (FindFaviconUrls(job, urls)) {
                for (const std::string& favicon : urls) {
                    std::vector<uint8_t> logo;
                    if (HttpGet(ToWide(favicon), logo, 6000, 2 * 1024 * 1024) && LooksLikeImage(logo)) {
                        WriteFile(img, logo);
                        image = std::make_shared<std::vector<uint8_t>>(std::move(logo));
                        state = LogoState::Found;
                        Logf("LOGO '%s': %s (%zu bytes)", job.name.c_str(), favicon.c_str(), image->size());
                        break;
                    }
                    Logf("LOGO '%s': %s is not an image, trying the next one", job.name.c_str(), favicon.c_str());
                }
            }
            if (state != LogoState::Found) {
                WriteFile(none, {});  // do not ask again for a week
                Logf("LOGO '%s': none found", job.name.c_str());
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Entry& e = g_cache[job.key];
    e.state = state;
    e.image = image;
}

const char kCustomReadme[] =
    "Own station pictures\r\n"
    "====================\r\n"
    "\r\n"
    "Put a picture here and name it like the station, for example:\r\n"
    "\r\n"
    "    Real Hardstyle.png\r\n"
    "\r\n"
    "The name is the station name from your live_streams.sii (upper/lower case does not matter). PNG, JPG, WEBP, GIF, BMP and\r\n"
    "ICO work. The picture is used instead of the logo from radio-browser.info; with cover_mode=logo in EtsNowPlaying.ini it is\r\n"
    "the only picture shown for that station. Restart the game after adding a file.\r\n";

void Run() {
    CreateDirectoryW((PluginDir() + L"\\logos").c_str(), nullptr);
    CreateDirectoryW((PluginDir() + L"\\logos\\custom").c_str(), nullptr);
    const std::wstring readme = PluginDir() + L"\\logos\\custom\\README.txt";
    if (GetFileAttributesW(readme.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(readme.c_str(), L"wb");
        if (f) {
            fwrite(kCustomReadme, 1, sizeof(kCustomReadme) - 1, f);
            fclose(f);
        }
    }
    while (!g_stop) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return g_stop || !g_queue.empty(); });
            if (g_stop) return;
            job = std::move(g_queue.front());
            g_queue.pop_front();
        }
        try {
            Process(job);
        } catch (...) {
            Logf("LOGO lookup raised an exception");
        }
    }
}

}  // namespace

void StationLogoStart() {
    g_stop = false;
    g_thread = std::thread(Run);
}

void StationLogoStop() {
    g_stop = true;
    g_cv.notify_all();
    if (g_thread.joinable()) g_thread.join();
}

void StationLogoRequest(const std::string& stationName, const std::string& country, const std::string& listUrl) {
    if (stationName.empty()) return;  // the lookup itself decides what may use the network
    const std::string key = KeyOf(stationName, listUrl);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cache.count(key)) return;
    g_cache[key] = Entry{};
    if (g_queue.size() < 8) g_queue.push_back({key, stationName, country, listUrl});
    g_cv.notify_one();
}

LogoResult StationLogoGet(const std::string& stationName, const std::string& listUrl) {
    LogoResult r;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(KeyOf(stationName, listUrl));
    if (it == g_cache.end()) return r;
    r.state = it->second.state;
    r.image = it->second.image;
    return r;
}

}  // namespace ep
