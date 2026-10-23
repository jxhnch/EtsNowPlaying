#include "radio_tracker.h"

#include "log.h"
#include "settings.h"
#include "station_db.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace ep {
namespace {

struct Conn {
    std::string host;   // normalized host[:port] from the Host header
    std::string path;   // request target
    bool wantsIcy = false;

    std::string hdr;
    bool hdrDone = false;
    int status = 0;
    int metaint = 0;
    std::string icyName, icyGenre, contentType, location, transferEncoding;
    bool isStream = false;
    int station = -1;
    const char* matchHow = "";

    int64_t untilMeta = 0;
    int metaLeft = 0;
    std::string metaBuf;
    uint64_t audioBytes = 0;
    bool loggedFirstAudio = false;
    ULONGLONG lastRecv = 0;     // tick of the last data from this stream
    ULONGLONG openedTick = 0;   // tick of the request
    ULONGLONG streamStart = 0;  // tick when the stream answered with audio
    ULONGLONG maxGapMs = 0;     // longest pause between two reads while playing (after the start-up burst)
    uint32_t reads = 0;
};

std::mutex g_mutex;
std::unordered_map<SOCKET, Conn> g_conns;
std::atomic<int> g_tracked{0};
StationDb g_stations;
// When a station answers with a redirect, the follow-up request gets the same station.
std::unordered_map<std::string, int> g_redirects;
SOCKET g_activeSock = INVALID_SOCKET;
RadioSnapshot g_snap;
int g_tlsLogged = 0;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// Escapes control characters so a header block fits on one log line.
std::string Printable(const std::string& s, size_t max = 700) {
    std::string out;
    for (size_t i = 0; i < s.size() && out.size() < max; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c < 32) out += '.';
        else out += static_cast<char>(c);
    }
    return out;
}

// Returns the value of header `name` (case-insensitive) or an empty string.
std::string HeaderValue(const std::string& block, const char* name) {
    std::string key = Lower(name);
    size_t pos = 0;
    while (pos < block.size()) {
        size_t eol = block.find("\r\n", pos);
        if (eol == std::string::npos) eol = block.size();
        std::string line = block.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos && Lower(Trim(line.substr(0, colon))) == key) return Trim(line.substr(colon + 1));
        pos = eol + 2;
    }
    return {};
}

void ParseSni(const char* d, int len, std::string& sni) {
    auto u8 = [&](int i) { return static_cast<uint8_t>(d[i]); };
    auto u16 = [&](int i) { return (u8(i) << 8) | u8(i + 1); };
    int p = 5 + 4 + 2 + 32;
    if (p >= len) return;
    p += 1 + u8(p);
    if (p + 2 > len) return;
    p += 2 + u16(p);
    if (p + 1 > len) return;
    p += 1 + u8(p);
    if (p + 2 > len) return;
    int extEnd = std::min(len, p + 2 + u16(p));
    p += 2;
    while (p + 4 <= extEnd) {
        int type = u16(p), size = u16(p + 2);
        p += 4;
        if (type == 0 && p + 5 <= len) {
            int nameLen = u16(p + 3);
            if (p + 5 + nameLen <= len) sni.assign(d + p + 5, static_cast<size_t>(nameLen));
            return;
        }
        p += size;
    }
}

void PublishLocked(const Conn& c, SOCKET s) {
    g_activeSock = s;
    g_snap.active = true;
    g_snap.stationIndex = c.station;
    g_snap.host = c.host;
    g_snap.path = c.path;
    g_snap.icyName = c.icyName;
    g_snap.genre = c.icyGenre;
    g_snap.stationName.clear();
    g_snap.country.clear();
    g_snap.bitrate.clear();
    if (c.station >= 0) {
        g_snap.stationName = g_stations.at(c.station).name;
        g_snap.genre = g_stations.at(c.station).genre;
        g_snap.country = g_stations.at(c.station).lang;
        g_snap.bitrate = g_stations.at(c.station).bitrate;
    } else {
        g_snap.stationName = c.icyName;
    }
    g_snap.title.clear();
    ++g_snap.titleSerial;
}

void ParseMetaBlockLocked(Conn& c, SOCKET s) {
    // StreamTitle='Artist - Song';StreamUrl='...';  (apostrophes inside titles are not escaped)
    const std::string key = "StreamTitle='";
    size_t b = c.metaBuf.find(key);
    if (b == std::string::npos) return;
    b += key.size();
    size_t e = c.metaBuf.find("';", b);
    if (e == std::string::npos) {
        e = c.metaBuf.find('\0', b);
        if (e == std::string::npos) e = c.metaBuf.size();
        if (e > b && c.metaBuf[e - 1] == '\'') --e;
    }
    std::string title = c.metaBuf.substr(b, e - b);
    if (s == g_activeSock && title != g_snap.title) {
        g_snap.title = title;
        ++g_snap.titleSerial;
        Logf("RADIO title [%s]: %s", g_snap.stationName.c_str(), title.c_str());
    } else if (s != g_activeSock) {
        Logf("RADIO title (inactive socket): %s", title.c_str());
    }
}

void ProcessBodyLocked(Conn& c, SOCKET s, const char* p, int n) {
    if (n <= 0) return;
    {
        ULONGLONG now = GetTickCount64();
        // The game reads the stream in regular steps while it plays: remember how long those steps get.
        if (c.lastRecv && c.streamStart && now - c.streamStart > 2000) c.maxGapMs = std::max<ULONGLONG>(c.maxGapMs, now - c.lastRecv);
        c.lastRecv = now;
        ++c.reads;
    }
    if (!c.loggedFirstAudio) {
        c.loggedFirstAudio = true;
        Logf("RADIO first body bytes: %d (metaint=%d)", n, c.metaint);
    }
    if (c.metaint <= 0) {
        c.audioBytes += static_cast<uint64_t>(n);
        return;
    }
    while (n > 0) {
        if (c.metaLeft > 0) {
            int take = std::min(n, c.metaLeft);
            c.metaBuf.append(p, static_cast<size_t>(take));
            p += take;
            n -= take;
            c.metaLeft -= take;
            if (c.metaLeft == 0) {
                ParseMetaBlockLocked(c, s);
                c.untilMeta = c.metaint;
            }
            continue;
        }
        if (c.untilMeta == 0) {
            int len = static_cast<uint8_t>(*p) * 16;
            ++p;
            --n;
            if (len == 0) {
                c.untilMeta = c.metaint;
            } else {
                c.metaLeft = len;
                c.metaBuf.clear();
            }
            continue;
        }
        int take = static_cast<int>(std::min<int64_t>(n, c.untilMeta));
        c.audioBytes += static_cast<uint64_t>(take);
        p += take;
        n -= take;
        c.untilMeta -= take;
    }
}

void OnHeadersCompleteLocked(Conn& c, SOCKET s) {
    size_t eol = c.hdr.find("\r\n");
    std::string statusLine = c.hdr.substr(0, eol);
    size_t sp = statusLine.find(' ');
    c.status = sp == std::string::npos ? 0 : atoi(statusLine.c_str() + sp + 1);
    c.metaint = atoi(HeaderValue(c.hdr, "icy-metaint").c_str());
    c.icyName = HeaderValue(c.hdr, "icy-name");
    c.icyGenre = HeaderValue(c.hdr, "icy-genre");
    c.contentType = HeaderValue(c.hdr, "content-type");
    c.location = HeaderValue(c.hdr, "location");
    c.transferEncoding = HeaderValue(c.hdr, "transfer-encoding");

    Logf("RADIO response [%s%s]: '%s' status=%d metaint=%d icy-name='%s' content-type='%s' location='%s' te='%s'",
         c.host.c_str(), c.path.c_str(), statusLine.c_str(), c.status, c.metaint, c.icyName.c_str(),
         c.contentType.c_str(), c.location.c_str(), c.transferEncoding.c_str());
    if (c.status != 200 || c.metaint > 0 || !c.icyName.empty() || Lower(c.contentType).rfind("audio/", 0) == 0) {
        Logf("RADIO raw response headers: %s", Printable(c.hdr).c_str());
    }

    if (c.status >= 300 && c.status < 400 && !c.location.empty()) {
        std::string hp, path;
        StationDb::SplitUrl(c.location, hp, path);
        if (c.station >= 0) {
            g_redirects[hp + path] = c.station;
            Logf("RADIO redirect remembered: %s%s -> station '%s'", hp.c_str(), path.c_str(),
                 g_stations.at(c.station).name.c_str());
        }
        return;
    }

    std::string ct = Lower(c.contentType);
    c.isStream = c.status == 200 && (c.metaint > 0 || !c.icyName.empty() || ct.rfind("audio/", 0) == 0 ||
                                     ct.find("mpeg") != std::string::npos || ct.find("ogg") != std::string::npos ||
                                     ct.find("aac") != std::string::npos);
    if (c.isStream) {
        c.untilMeta = c.metaint;
        c.lastRecv = GetTickCount64();
        c.streamStart = c.lastRecv;
        PublishLocked(c, s);
        Logf("RADIO ACTIVE: station='%s' (%s) genre='%s' icy-name='%s'", g_snap.stationName.c_str(), c.matchHow,
             g_snap.genre.c_str(), c.icyName.c_str());
    }
}

}  // namespace

void RadioTrackerInit() {
    std::wstring file = GameDocsDir() + L"\\live_streams.sii";
    if (g_stations.Load(file)) {
        Logf("RADIO station list: %zu stations loaded from live_streams.sii", g_stations.size());
    } else {
        Logf("RADIO station list: could not load live_streams.sii");
    }
}

void RadioTrackerShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_conns.clear();
    g_tracked = 0;
    g_snap = RadioSnapshot{};
}

void RadioOnSend(SOCKET s, const char* data, int len) {
    if (len >= 6 && static_cast<uint8_t>(data[0]) == 0x16 && data[1] == 3 && data[5] == 1) {
        // TLS ClientHello: tells us whether the game itself speaks https to radio hosts.
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_tlsLogged < 300) {
            ++g_tlsLogged;
            std::string sni;
            ParseSni(data, len, sni);
            Logf("NET TLS ClientHello sock=%llu SNI='%s'", static_cast<unsigned long long>(s), sni.c_str());
        }
        return;
    }
    if (len < 14 || memcmp(data, "GET ", 4) != 0) return;

    std::string req(data, static_cast<size_t>(std::min(len, 4096)));
    if (req.find(" HTTP/1.") == std::string::npos) return;

    size_t sp2 = req.find(' ', 4);
    if (sp2 == std::string::npos) return;

    Conn c;
    c.path = req.substr(4, sp2 - 4);
    std::string hostHeader = HeaderValue(req, "host");
    c.host = StationDb::NormalizeHostPort(hostHeader);
    c.wantsIcy = !HeaderValue(req, "icy-metadata").empty();

    std::lock_guard<std::mutex> lock(g_mutex);
    const char* how = "none";
    auto redirect = g_redirects.find(c.host + c.path);
    if (redirect != g_redirects.end()) {
        c.station = redirect->second;
        how = "via-redirect";
    } else {
        c.station = g_stations.Match(c.host, c.path, &how);
    }
    c.matchHow = how;
    c.openedTick = GetTickCount64();
    Logf("RADIO request sock=%llu host='%s' path='%s' icy=%d station=%s (%s)", static_cast<unsigned long long>(s),
         c.host.c_str(), c.path.c_str(), c.wantsIcy ? 1 : 0,
         c.station >= 0 ? g_stations.at(c.station).name.c_str() : "?", how);
    if (c.wantsIcy || c.station >= 0) Logf("RADIO raw request: %s", Printable(req).c_str());

    if (g_conns.find(s) == g_conns.end()) ++g_tracked;
    g_conns[s] = std::move(c);
}

void RadioOnRecv(SOCKET s, const char* data, int len) {
    if (g_tracked.load(std::memory_order_relaxed) == 0 || len <= 0) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_conns.find(s);
    if (it == g_conns.end()) return;
    Conn& c = it->second;

    if (!c.hdrDone) {
        c.hdr.append(data, static_cast<size_t>(len));
        size_t end = c.hdr.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (c.hdr.size() > 16384) {
                Logf("RADIO response without header end, dropping sock=%llu: %s",
                     static_cast<unsigned long long>(s), Printable(c.hdr, 200).c_str());
                g_conns.erase(it);
                --g_tracked;
            }
            return;
        }
        std::string body = c.hdr.substr(end + 4);
        c.hdr.resize(end + 2);
        c.hdrDone = true;
        OnHeadersCompleteLocked(c, s);
        if (c.isStream) ProcessBodyLocked(c, s, body.data(), static_cast<int>(body.size()));
        return;
    }
    if (c.isStream) ProcessBodyLocked(c, s, data, len);
}

void RadioOnClose(SOCKET s) {
    if (g_tracked.load(std::memory_order_relaxed) == 0) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_conns.find(s);
    if (it == g_conns.end()) return;
    Logf("RADIO socket closed sock=%llu host='%s' audioBytes=%llu reads=%u maxReadGap=%llums", static_cast<unsigned long long>(s),
         it->second.host.c_str(), static_cast<unsigned long long>(it->second.audioBytes), it->second.reads,
         static_cast<unsigned long long>(it->second.maxGapMs));
    if (s == g_activeSock) {
        g_activeSock = INVALID_SOCKET;
        g_snap.active = false;
        ++g_snap.titleSerial;
        Logf("RADIO INACTIVE (stream closed)");
    }
    g_conns.erase(it);
    --g_tracked;
}

RadioSnapshot RadioGetSnapshot() {
    static bool stale = false;
    std::lock_guard<std::mutex> lock(g_mutex);
    RadioSnapshot s = g_snap;
    const ULONGLONG now = GetTickCount64();
    if (s.active) {
        // The game keeps the stream open while the radio is paused, it just stops reading from it.
        // Allow for the longest read step seen so far, but never less than the configured minimum.
        auto it = g_conns.find(g_activeSock);
        ULONGLONG age = 0, limit = static_cast<ULONGLONG>(GetSettings().radioStaleMs);
        if (it != g_conns.end()) {
            s.ageMs = static_cast<uint32_t>(std::min<ULONGLONG>(now - it->second.streamStart, 3600000));
            age = now - it->second.lastRecv;
            limit = std::min<ULONGLONG>(std::max<ULONGLONG>(limit, it->second.maxGapMs * 3), 8000);
        }
        bool nowStale = age > limit;
        if (nowStale != stale) {
            stale = nowStale;
            Logf("RADIO %s (no stream data for %llu ms, limit %llu ms, longest normal gap %llu ms)",
                 nowStale ? "PAUSED" : "RESUMED", static_cast<unsigned long long>(age), static_cast<unsigned long long>(limit),
                 it != g_conns.end() ? static_cast<unsigned long long>(it->second.maxGapMs) : 0ull);
        }
        if (nowStale) s.active = false;
    } else {
        stale = false;
    }
    // A station switch closes the old stream before the new one answers.
    for (const auto& kv : g_conns) {
        const Conn& c = kv.second;
        bool pending = !c.hdrDone || (c.status >= 300 && c.status < 400);
        if (c.wantsIcy && !c.isStream && pending && now - c.openedTick < 4000) {
            s.connecting = true;
            break;
        }
    }
    return s;
}

}  // namespace ep
