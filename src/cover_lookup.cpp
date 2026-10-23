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

// What we are looking for, in several spellings.
struct Wanted {
    std::wstring artist;       // as sent by the station
    std::wstring firstArtist;  // "A & B feat. C" -> "A"
    std::wstring track;        // without brackets / "- Remastered"
    std::wstring artistKey, firstKey, trackKey;
};

// A hit from a catalogue is accepted when the track matches and at least the main artist does.
bool Accept(const Wanted& w, const std::string& gotArtist, const std::string& gotTrack) {
    std::wstring ga = NormalizeKey(ToWide(gotArtist));
    std::wstring gt = NormalizeKey(BaseTitle(ToWide(gotTrack)));
    if (!SimilarKeys(gt, w.trackKey)) return false;
    return SimilarKeys(ga, w.artistKey) || SimilarKeys(ga, w.firstKey);
}

bool Download(const std::string& url, std::shared_ptr<const std::vector<uint8_t>>& image) {
    std::vector<uint8_t> img;
    if (!HttpGet(ToWide(url), img, 8000, 4u * 1024 * 1024) || img.empty()) {
        Logf("COVER download failed: %s", url.c_str());
        return false;
    }
    image = std::make_shared<std::vector<uint8_t>>(std::move(img));
    return true;
}

// iTunes Search API: {"results":[{"wrapperType":"track","artistName":"..","trackName":"..","artworkUrl100":".."}, ...]}
bool SearchITunes(const Wanted& w, const std::wstring& artistTerm, std::shared_ptr<const std::vector<uint8_t>>& image,
                  std::string& found) {
    std::string term = UrlEncode(ToUtf8(artistTerm + L" " + w.track));
    std::wstring url = L"https://itunes.apple.com/search?media=music&entity=song&limit=10&term=" +
                       std::wstring(term.begin(), term.end());
    std::vector<uint8_t> body;
    if (!HttpGet(url, body, 6000, 1024 * 1024)) return false;
    std::string json(body.begin(), body.end());

    size_t pos = 0;
    while ((pos = json.find("{\"wrapperType\"", pos)) != std::string::npos) {
        size_t next = json.find("{\"wrapperType\"", pos + 1);
        std::string obj = json.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? json.size() : next;

        std::string artist, track, art;
        if (!JsonString(obj, "artistName", artist) || !JsonString(obj, "trackName", track) ||
            !JsonString(obj, "artworkUrl100", art) || !Accept(w, artist, track)) {
            continue;
        }
        size_t size = art.find("100x100");
        if (size != std::string::npos) art.replace(size, 7, "600x600");
        if (!Download(art, image)) return false;
        found = "iTunes: '" + track + "' by '" + artist + "'";
        return true;
    }
    return false;
}

// Deezer: {"data":[{"id":..,"readable":true,"title":"..","artist":{"name":".."},"album":{"cover_big":".."}}, ...]}
bool SearchDeezer(const Wanted& w, bool exactFields, std::shared_ptr<const std::vector<uint8_t>>& image, std::string& found) {
    std::wstring query = exactFields ? L"artist:\"" + w.firstArtist + L"\" track:\"" + w.track + L"\""
                                     : w.firstArtist + L" " + w.track;
    std::string q = UrlEncode(ToUtf8(query));
    std::wstring url = L"https://api.deezer.com/search?limit=10&q=" + std::wstring(q.begin(), q.end());
    std::vector<uint8_t> body;
    if (!HttpGet(url, body, 6000, 1024 * 1024)) return false;
    std::string json(body.begin(), body.end());

    size_t pos = 0;
    while ((pos = json.find("\"readable\":", pos)) != std::string::npos) {
        size_t next = json.find("\"readable\":", pos + 1);
        std::string obj = json.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? json.size() : next;

        std::string track, artist, art;
        if (!JsonString(obj, "title", track)) continue;
        size_t a = obj.find("\"artist\":{");
        if (a == std::string::npos || !JsonString(obj.substr(a), "name", artist)) continue;
        if (!JsonString(obj, "cover_big", art) && !JsonString(obj, "cover_medium", art)) continue;
        if (!Accept(w, artist, track)) continue;
        if (!Download(art, image)) return false;
        found = "Deezer: '" + track + "' by '" + artist + "'";
        return true;
    }
    return false;
}

bool Lookup(const std::string& streamTitle, std::shared_ptr<const std::vector<uint8_t>>& image) {
    std::wstring artist, track;
    if (!SplitArtistTitle(streamTitle, artist, track)) return false;  // no "Artist - Song": nothing to search for

    Wanted w;
    w.artist = artist;
    w.firstArtist = FirstArtist(artist);
    w.track = BaseTitle(track);
    w.artistKey = NormalizeKey(artist);
    w.firstKey = NormalizeKey(w.firstArtist);
    w.trackKey = NormalizeKey(w.track);
    if (w.firstKey.empty() || w.trackKey.empty()) return false;

    // Several tries, strict to loose, over two catalogues. The first hit with a matching artist and track wins.
    std::string found;
    bool ok = SearchITunes(w, w.firstArtist, image, found) || SearchDeezer(w, true, image, found) ||
              SearchDeezer(w, false, image, found) ||
              (w.firstArtist != w.artist && SearchITunes(w, w.artist, image, found));
    if (ok) Logf("COVER found for '%s' -> %s (%zu bytes)", streamTitle.c_str(), found.c_str(), image->size());
    else Logf("COVER no match for '%s'", streamTitle.c_str());
    return ok;
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
