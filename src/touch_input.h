#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include "touch_frame.h"

struct TouchStats {
    bool available = false;
    uint8_t address = 0;
    uint32_t frames = 0;
    uint32_t errors = 0;
    uint32_t invalid_frames = 0;
    uint32_t recoveries = 0;
};

void touch_input_init(int sda, int scl, uint16_t width, uint16_t height);
bool touch_input_read(TouchPoint &point);
const TouchStats &touch_input_stats();

#endif
