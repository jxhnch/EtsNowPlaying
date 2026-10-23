#pragma once
#include "common.h"

#include <d3d11.h>
#include <dxgi.h>

#include <functional>

namespace ep {

// Use the mouse on the cabin screen.
//
// Picking: while the game draws the screen mesh with our texture, the same draw call is repeated into a
// 1x1 render target that only covers the pixel under the mouse cursor, with a probe texture bound whose
// texels store their own texture coordinates. Reading that pixel back tells where on the screen the mouse is.

struct TouchState {
    bool hover = false;       // the mouse is on the screen right now
    float x = 0, y = 0;       // position in screen-image pixels (800x480, origin top-left)
    uint64_t clickTick = 0;   // GetTickCount64() of the last click on the screen, 0 = never
    float clickX = 0, clickY = 0;
};

using PsSetSrvFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);

void TouchInit(bool enabled, PsSetSrvFn originalPsSetSrv);

// Called when the view of our screen texture changes (the game recreated the screen).
void TouchSetScreenView(ID3D11ShaderResourceView* ours);

// PSSetShaderResources bookkeeping (immediate context only): isScreen[i] says whether slot start+i got our view.
void TouchNoteBind(ID3D11DeviceContext* ctx, UINT start, UINT count, const bool* isScreen);
bool TouchHasBindings();
bool TouchNeedsReplay(ID3D11DeviceContext* ctx);
void TouchReplay(ID3D11DeviceContext* ctx, const std::function<void()>& drawOnce);

// Once per frame, from the Present hook.
void TouchOnPresent(IDXGISwapChain* sc);

// Whether the screen currently shows buttons (set by the renderer).
void TouchSetButtonsVisible(bool visible);
TouchState TouchGetState();

}  // namespace ep
