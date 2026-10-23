#include "station_db.h"

#include "log.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ep {
namespace {

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

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Undoes the escapes the game writes into .sii strings (\xHH, \", \\).
std::string Unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'x' && i + 3 < s.size() && HexVal(s[i + 2]) >= 0 && HexVal(s[i + 3]) >= 0) {
                out.push_back(static_cast<char>(HexVal(s[i + 2]) * 16 + HexVal(s[i + 3])));
                i += 3;
                continue;
            }
            if (n == '"' || n == '\\') {
                out.push_back(n);
                ++i;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string StripQuery(const std::string& path) {
    size_t q = path.find('?');
    return q == std::string::npos ? path : path.substr(0, q);
}

std::string HostOnly(const std::string& hostPort) {
    size_t colon = hostPort.rfind(':');
    return colon == std::string::npos || hostPort.find(']') != std::string::npos ? hostPort : hostPort.substr(0, colon);
}

}  // namespace

std::string StationDb::NormalizeHostPort(std::string hostPort) {
    hostPort = Lower(Trim(hostPort));
    for (const char* suffix : {":80", ":443"}) {
        size_t n = std::char_traits<char>::length(suffix);
        if (hostPort.size() > n && hostPort.compare(hostPort.size() - n, n, suffix) == 0) {
            hostPort.erase(hostPort.size() - n);
            break;
        }
    }
    return hostPort;
}

void StationDb::SplitUrl(const std::string& urlIn, std::string& hostPort, std::string& path) {
    std::string url = Trim(urlIn);
    size_t scheme = url.find("://");
    if (scheme != std::string::npos) url.erase(0, scheme + 3);
    size_t slash = url.find('/');
    hostPort = NormalizeHostPort(slash == std::string::npos ? url : url.substr(0, slash));
    path = slash == std::string::npos ? "/" : url.substr(slash);
}

bool StationDb::Load(const std::wstring& file) {
    stations_.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.find("stream_data[") == std::string::npos) continue;
        size_t q1 = line.find('"');
        size_t q2 = line.rfind('"');
        if (q1 == std::string::npos || q2 <= q1) continue;
        std::string raw = Unescape(line.substr(q1 + 1, q2 - q1 - 1));

        std::vector<std::string> f;
        std::stringstream ss(raw);
        std::string part;
        while (std::getline(ss, part, '|')) f.push_back(part);
        if (f.size() < 2) continue;

        Station st;
        st.url = Trim(f[0]);
        st.name = f[1];
        if (f.size() > 2) st.genre = f[2];
        if (f.size() > 3) st.lang = f[3];
        if (f.size() > 4) st.bitrate = f[4];
        if (f.size() > 5) st.favorite = Trim(f[5]) == "1";
        SplitUrl(st.url, st.hostPort, st.path);
        stations_.push_back(std::move(st));
    }
    return !stations_.empty();
}

int StationDb::Match(const std::string& hostPortIn, const std::string& pathIn, const char** how) const {
    std::string hostPort = NormalizeHostPort(hostPortIn);
    std::string path = Trim(pathIn);
    if (path.empty()) path = "/";

    auto setHow = [&](const char* h) { if (how) *how = h; };

    for (size_t i = 0; i < stations_.size(); ++i) {
        if (stations_[i].hostPort == hostPort && stations_[i].path == path) {
            setHow("exact");
            return static_cast<int>(i);
        }
    }
    std::string pathNoQuery = StripQuery(path);
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (stations_[i].hostPort == hostPort && StripQuery(stations_[i].path) == pathNoQuery) {
            setHow("path-no-query");
            return static_cast<int>(i);
        }
    }
    // The game's audio engine leaves the port out of the Host header, so compare host names only.
    const std::string host = HostOnly(hostPort);
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (HostOnly(stations_[i].hostPort) == host && StripQuery(stations_[i].path) == pathNoQuery) {
            setHow("host-and-path");
            return static_cast<int>(i);
        }
    }
    int only = -1, count = 0;
    for (size_t i = 0; i < stations_.size(); ++i) {
        if (HostOnly(stations_[i].hostPort) == host) {
            only = static_cast<int>(i);
            ++count;
        }
    }
    if (count == 1) {
        setHow("host-only-unique");
        return only;
    }
    setHow(count > 1 ? "host-ambiguous" : "none");
    return -1;
}

}  // namespace ep
