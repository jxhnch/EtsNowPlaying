#pragma once
#include "common.h"

namespace ep {

// Size of the picture shown on the cabin screen.
constexpr uint32_t kScreenWidth = 800;
constexpr uint32_t kScreenHeight = 480;

struct FrameView {
    const uint8_t* data = nullptr;  // BGRA, premultiplied, opaque
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint64_t version = 0;
};

// Starts the thread that draws the screen (Direct2D / DirectWrite into a CPU bitmap, ~15 fps).
void RendererStart();
void RendererStop();

// If a frame newer than `afterVersion` exists, fills `view` and returns true with the frame locked.
// The caller must then call RendererUnlockFrame().
bool RendererLockFrame(uint64_t afterVersion, FrameView* view);
void RendererUnlockFrame();

}  // namespace ep
