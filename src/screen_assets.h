#pragma once
#include "common.h"

namespace ep {

// Size of the placeholder texture. The D3D11 hook recognises the screen by this unusual size.
constexpr UINT kPlaceholderWidth = 64;
constexpr UINT kPlaceholderHeight = 2048;

// Writes gps.tobj / gps.dds into `dir` if they are missing. The game reads them through
// "/home/<folder>/gps.tobj" because its documents folder is mounted at /home.
bool EnsureScreenAssets(const std::wstring& dir);

}  // namespace ep
