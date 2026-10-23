#pragma once
#include "common.h"

namespace ep {

// Searches the executable sections of `module` for a byte pattern such as "48 8B ?? 05".
// Returns the address of the single match, or nullptr if there is none or more than one.
// `hits` (optional) receives the number of matches found.
uint8_t* FindUniquePattern(HMODULE module, const char* pattern, int* hits = nullptr);

}  // namespace ep
