#pragma once
#include "common.h"

namespace ep {

// Hooks the Winsock calls the game's audio engine uses to stream internet radio.
bool NetHooksInstall();
void NetHooksDisable();

}  // namespace ep
