#pragma once
#include "common.h"

#include <vector>

namespace ep {

struct Station {
    std::string url;
    std::string name;
    std::string genre;
    std::string lang;
    std::string bitrate;
    bool favorite = false;
    std::string hostPort;  // normalized, lower case, default ports removed
    std::string path;      // starts with '/'
};

// The user's radio list, read from live_streams.sii.
class StationDb {
public:
    bool Load(const std::wstring& file);
    size_t size() const { return stations_.size(); }
    const Station& at(int i) const { return stations_[static_cast<size_t>(i)]; }

    // Returns the station index or -1. `how` describes which rule matched.
    int Match(const std::string& hostPort, const std::string& path, const char** how) const;

    static void SplitUrl(const std::string& url, std::string& hostPort, std::string& path);
    static std::string NormalizeHostPort(std::string hostPort);

private:
    std::vector<Station> stations_;
};

}  // namespace ep
