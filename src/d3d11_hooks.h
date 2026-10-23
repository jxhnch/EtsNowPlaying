#pragma once
#include "common.h"

namespace ep {

// Hooks Present / PSSetShaderResources / CreateShaderResourceView of the game's D3D11 device.
// replaceTexture: swap the 64x2048 placeholder for our screen texture. uploadFrames: fill it every frame.
bool D3D11HooksInstall(bool replaceTexture, bool uploadFrames);
void D3D11HooksDisable();

}  // namespace ep
