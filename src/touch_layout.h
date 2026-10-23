#pragma once

namespace ep {

// Touch buttons on the Spotify screen, in screen-image pixels (800x480, origin top-left).
enum class Button { None, Previous, PlayPause, Next };

struct ButtonSpec {
    Button id;
    float cx, cy, r;
};

inline constexpr ButtonSpec kButtons[] = {
    {Button::Previous, 384.0f, 420.0f, 22.0f},
    {Button::PlayPause, 446.0f, 420.0f, 26.0f},
    {Button::Next, 508.0f, 420.0f, 22.0f},
};

// Progress bar of the Spotify screen: starts at kBarX, its right end follows the width of the layout canvas.
inline constexpr float kBarX = 360.0f, kBarY = 330.0f, kBarH = 8.0f, kBarRightMargin = 40.0f;

inline float BarWidth(float canvasW) { return canvasW - kBarX - kBarRightMargin; }

// A generous strip around the bar (the labels below it are not part of it).
inline bool HitProgressBar(float x, float y, float canvasW) {
    return x >= kBarX - 16.0f && x <= kBarX + BarWidth(canvasW) + 16.0f && y >= kBarY - 20.0f && y <= kBarY + kBarH + 16.0f;
}

// Finger-friendly: the hit area is a bit larger than the drawn circle.
inline Button HitTestButtons(float x, float y) {
    for (const ButtonSpec& b : kButtons) {
        float dx = x - b.cx, dy = y - b.cy, reach = b.r + 12.0f;
        if (dx * dx + dy * dy <= reach * reach) return b.id;
    }
    return Button::None;
}

}  // namespace ep
