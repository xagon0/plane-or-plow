#ifndef TOUCH_FRAME_H
#define TOUCH_FRAME_H

#include <stddef.h>
#include <stdint.h>

struct TouchPoint { uint16_t x, y; };
enum class TouchFrameResult { NotReady, Released, Pressed, Invalid };

// Only the first GT911 contact is needed by LVGL. The packet begins at 0x8150
// (X low/high, Y low/high), after the tracking ID. Never index by touch count.
inline TouchFrameResult parse_touch_frame(uint8_t status, const uint8_t *xy,
                                         size_t size, uint16_t width,
                                         uint16_t height, TouchPoint &point) {
    if (!(status & 0x80)) return TouchFrameResult::NotReady;
    const uint8_t count = status & 0x0f;
    if (count > 5) return TouchFrameResult::Invalid;
    if (count == 0) return TouchFrameResult::Released;
    if (!xy || size != 4 || !width || !height) return TouchFrameResult::Invalid;
    const uint16_t x = uint16_t(xy[0]) | (uint16_t(xy[1]) << 8);
    const uint16_t y = uint16_t(xy[2]) | (uint16_t(xy[3]) << 8);
    if (x >= width || y >= height) return TouchFrameResult::Invalid;
    // The old ROTATION_INVERTED setting passed raw X/Y through unchanged.
    point = {x, y};
    return TouchFrameResult::Pressed;
}

#endif
