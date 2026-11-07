#pragma once
#include "common.h"

namespace ep {

// Which parts of the screen feature are active. Normally everything; the stages are a developer switch
// ([features] force_stage in the INI) that turns parts off one at a time when hunting for a problem.
struct Features {
    int stage = 0;               // 0 = everything on ... 4 = display fully off (diagnostics only)
    bool screen = true;          // redirect the GPS texture and replace it with ours
    bool disableGameGps = true;  // stop the game from drawing its own GPS UI
    bool upload = true;          // copy rendered frames into the texture
    bool touch = true;           // beta: mouse input on the screen
};

// Reads state.ini / EtsNowPlaying.ini in `dir`. If the previous run did not shut down cleanly the
// stage is raised by one. Marks this run as "running".
Features SafeModeBegin(const std::wstring& dir);
// Marks a clean shutdown.
void SafeModeEndClean();

}  // namespace ep
