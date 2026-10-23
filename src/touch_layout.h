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

// Finger-friendly: the hit area is a bit larger than the drawn circle.
inline Button HitTestButtons(float x, float y) {
    for (const ButtonSpec& b : kButtons) {
        float dx = x - b.cx, dy = y - b.cy, reach = b.r + 12.0f;
        if (dx * dx + dy * dy <= reach * reach) return b.id;
    }
    return Button::None;
}

}  // namespace ep
