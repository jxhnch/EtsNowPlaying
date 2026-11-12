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

// Puts the game's own GPS UI back (takes effect the next time a cabin is built).
void EngineRestoreGameGps();

// Observe-only: the hooks count lookups of the GPS texture but redirect nothing (used while a game version is flagged).
void EngineSetObserveOnly(bool on);
int EngineRedirects();      // GPS lookups redirected to our placeholder in this session
int EngineGpsSightings();  // GPS lookups seen, redirected or not
// True if a GPS lookup was redirected to our placeholder within the last `ms` milliseconds. Only then can a texture that
// is created belong to our placeholder file.
bool EngineRedirectRecent(unsigned ms);

}  // namespace ep
