#pragma once
#include "common.h"

namespace ep {

// User settings from <plugin folder>\EtsNowPlaying.ini. Missing keys are added with their defaults.
struct Settings {
    bool flipVertical = true;       // [screen] flip_vertical: the game samples the screen texture upside down
    std::string appFilter = "spotify";  // [media] app_filter: only show media from apps containing this text ("" = any)
    bool coverLookup = true;        // [radio] cover_lookup: look up song covers for radio titles on iTunes
    int radioStaleMs = 2500;        // [radio] pause_after_ms: no stream data for this long = radio paused
    int radioHoldMs = 600;          // [radio] switch_hold_ms: keep the radio screen this long after a stream ends
    bool touch = true;              // [touch] enabled: click the buttons on the screen with the mouse
    bool autoFit = true;            // [screen] auto_fit: fit the picture to the shape of each truck's display
    bool textureFinder = false;     // [debug] texture_finder: developer tool (Ctrl+Shift+F6..F9), not in the default ini
};

void LoadSettings(const std::wstring& dir);
const Settings& GetSettings();

}  // namespace ep
