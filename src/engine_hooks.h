#pragma once
#include "common.h"

namespace ep {

// Hooks into the game's own code (found by byte signatures, so they survive most updates):
//  * the texture load queue: requests for the cabin GPS texture are redirected to our placeholder file
//  * the cabin "GPSWindow" renderer: switched off once our screen texture is in place
// `overridePath` is the virtual path of our placeholder, e.g. "/home/EtsNowPlaying/gps.tobj".
bool EngineHooksInstall(const char* overridePath, bool disableGameGps);
void EngineHooksDisable();

// Stops the game from drawing its own GPS UI (done at install time when requested).
void EngineDisableGameGps();

}  // namespace ep
