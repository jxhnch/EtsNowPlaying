#pragma once
#include "common.h"

namespace ep {

// Hooks Present / PSSetShaderResources / CreateShaderResourceView of the game's D3D11 device.
// replaceTexture: swap the 64x2048 placeholder for our screen texture. uploadFrames: fill it every frame.
// touch (beta): hook the draw calls so the mouse position on the cabin screen can be found.
bool D3D11HooksInstall(bool replaceTexture, bool uploadFrames, bool touch);
void D3D11HooksDisable();

}  // namespace ep
