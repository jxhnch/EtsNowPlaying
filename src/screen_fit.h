#pragma once
#include "common.h"

#include <d3d11.h>

namespace ep {

// Fits the picture onto the real display of whatever truck is driven, without any per-truck list.
//
// 1. Shape: while the game draws the display mesh, its vertex data is read back (positions and texture
//    coordinates). From it we get which part of the texture is visible and how big one texture unit is in each
//    direction, i.e. the true aspect ratio of the display.
// 2. Orientation: a coarse render of the screen's texture coordinates (see touch.cpp) tells which way the texture
//    runs on the screen: rotated, mirrored or upside down.
// 3. Both are combined into one affine transform from the 800x480 layout canvas to texture pixels. If a step fails,
//    the plain mapping (canvas fills the texture, flip_vertical setting) stays in use and the reason is logged.

struct ScreenFit {
    bool valid = false;
    // x' = a*x + c*y + e, y' = b*x + d*y + f  (indices a b c d e f)
    double canvasToTexel[6] = {};
    double texelToCanvas[6] = {};
    bool fromGeometry = false;  // false: orientation known but the real shape not, the picture fills the visible area
};

struct ScreenOrientation {
    bool ok = false;
    int rightAxis = 0;  // texture axis (0 = u, 1 = v) that runs to the right on the screen
    int rightSign = 1;  // +1 if it grows to the right
    int upAxis = 1;
    int upSign = 1;     // +1 if it grows upwards
};

// mode: 0 = off, 1 = measure and apply, 2 = measure and write the result to the log only
void FitInit(int mode);
bool FitEnabled();
void FitReset(const char* reason);  // a new screen texture appeared (new truck / reload)

void FitOnCreateInputLayout(ID3D11InputLayout* layout, const D3D11_INPUT_ELEMENT_DESC* elements, UINT count);

bool FitNeedsGeometry();
// Called from the draw hook for a draw call that shows our screen texture.
void FitCaptureGeometry(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex);

bool FitNeedsOrientation();
void FitNoteOrientation(const ScreenOrientation& o);

ScreenFit FitGet();

}  // namespace ep
