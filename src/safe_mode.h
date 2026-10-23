#pragma once
#include "common.h"

namespace ep {

// Which parts of the screen feature are active. After a crash the plugin switches parts off one
// step at a time, so a bad interaction with a game version or driver can never lock the user out.
struct Features {
    int stage = 0;               // 0 = everything on ... 3 = display fully off (diagnostics only)
    bool screen = true;          // redirect the GPS texture and replace it with ours
    bool disableGameGps = true;  // stop the game from drawing its own GPS UI
    bool upload = true;          // copy rendered frames into the texture
};

// Reads state.ini / EtsNowPlaying.ini in `dir`. If the previous run did not shut down cleanly the
// stage is raised by one. Marks this run as "running".
Features SafeModeBegin(const std::wstring& dir);
// Marks a clean shutdown.
void SafeModeEndClean();

}  // namespace ep
