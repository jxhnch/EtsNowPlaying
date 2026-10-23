#pragma once
#include "common.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace ep {

// What the Windows media controls (SMTC) report about the current player, e.g. Spotify.
struct MediaSnapshot {
    bool hasSession = false;
    std::string app;      // e.g. "Spotify.exe"
    std::string title;
    std::string artist;
    std::string album;
    bool playing = false;
    double duration = 0;  // seconds, 0 if unknown
    double position = 0;  // seconds at `sampledAt`
    std::chrono::steady_clock::time_point sampledAt;
    uint64_t trackSerial = 0;                          // changes with every new track
    uint64_t coverSerial = 0;                          // changes whenever `cover` changes
    std::shared_ptr<const std::vector<uint8_t>> cover;  // encoded image (PNG/JPEG), may be empty

    // Position right now, extrapolated while playing.
    double PositionNow() const;
};

void MediaStart();
void MediaStop();
MediaSnapshot MediaGetSnapshot();

// Playback control of the current media app (executed on the media thread).
enum class MediaCommand { None, Previous, PlayPause, Next };
void MediaSendCommand(MediaCommand cmd);

}  // namespace ep
