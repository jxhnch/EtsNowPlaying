#pragma once
#include "common.h"

#include <string>

namespace ep {

// What the game's radio is playing right now, as seen on the wire.
struct RadioSnapshot {
    bool active = false;        // a radio stream is open and delivering data
    bool connecting = false;    // a radio stream is being opened right now (e.g. station switch)
    int stationIndex = -1;      // index into the user's live_streams.sii, or -1
    std::string stationName;    // from live_streams.sii, falls back to icy-name
    std::string genre;
    std::string country;
    std::string bitrate;
    std::string stationUrl;     // the station's URL in live_streams.sii (empty if the station is not in the list)
    std::string host;
    std::string path;
    std::string icyName;
    std::string title;          // current StreamTitle, raw as sent by the station
    uint64_t titleSerial = 0;   // increments whenever the title changes
    uint32_t ageMs = 0;         // how long this stream has been delivering audio
};

void RadioTrackerInit();
void RadioTrackerShutdown();

// Called from the socket hooks. Both are cheap for traffic that is not an HTTP GET stream.
void RadioOnSend(SOCKET s, const char* data, int len);
void RadioOnRecv(SOCKET s, const char* data, int len);
void RadioOnClose(SOCKET s);

RadioSnapshot RadioGetSnapshot();

}  // namespace ep
