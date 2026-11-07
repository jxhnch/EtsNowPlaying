#pragma once
#include "common.h"

#include "radio_tracker.h"

namespace ep {

// Optional extra "now playing" sources, listed in <plugin folder>\EtsNowPlaying.sources.ini.
//
// Some stations only send the name of a show in their stream, not the song. If such a station offers the current song
// through a web address (JSON), an entry in the sources file tells the plugin where to ask. It asks only while that
// station is playing, and falls back to the stream's own title whenever the address gives nothing.

// Writes the sources file if it is missing, loads it and starts the background thread (if any entry is enabled).
void ExtraSourcesStart();
void ExtraSourcesStop();

// Called for every radio snapshot: replaces the title if a source is active for the station and has a song.
void ExtraSourcesApply(RadioSnapshot& s);

}  // namespace ep
