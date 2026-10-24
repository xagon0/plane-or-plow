#include "../src/touch_frame.h"
#include <assert.h>
#include <stdio.h>
#include <initializer_list>

int main() {
    const uint8_t valid[] = {0xdf, 0x01, 0, 0}; // x=479, y=0
    TouchPoint point = {123, 456};
    unsigned checks = 0;
    // All possible status bytes, including the old failed-read value 0xff.
    for (unsigned status = 0; status <= 255; ++status) {
        const TouchFrameResult result = parse_touch_frame(status, valid, 4, 480, 480, point);
        if (!(status & 0x80)) assert(result == TouchFrameResult::NotReady);
        else if ((status & 15) == 0) assert(result == TouchFrameResult::Released);
        else if ((status & 15) > 5) assert(result == TouchFrameResult::Invalid);
        else {
            assert(result == TouchFrameResult::Pressed);
            assert(point.x == 479 && point.y == 0); // unchanged board orientation
        }
        ++checks;
    }
    for (size_t size = 0; size <= 8; ++size) {
        const auto result = parse_touch_frame(0x81, valid, size, 480, 480, point);
        assert(result == (size == 4 ? TouchFrameResult::Pressed : TouchFrameResult::Invalid));
        ++checks;
    }
    assert(parse_touch_frame(0x81, nullptr, 4, 480, 480, point) == TouchFrameResult::Invalid);
    const uint8_t outside_x[] = {0xe0, 1, 0, 0}; // x=480 is outside
    const uint8_t outside_y[] = {0, 0, 0xe0, 1};
    const uint8_t failed_read[] = {0xff, 0xff, 0xff, 0xff};
    point = {123, 456};
    for (const auto *bytes : {outside_x, outside_y, failed_read}) {
        assert(parse_touch_frame(0x81, bytes, 4, 480, 480, point) == TouchFrameResult::Invalid);
        assert(point.x == 123 && point.y == 456);
        ++checks;
    }
    assert(parse_touch_frame(0xff, nullptr, 0, 480, 480, point) == TouchFrameResult::Invalid);
    assert(parse_touch_frame(0x80, nullptr, 0, 480, 480, point) == TouchFrameResult::Released);
    assert(parse_touch_frame(0, nullptr, 0, 480, 480, point) == TouchFrameResult::NotReady);
    assert(parse_touch_frame(0x81, valid, 4, 0, 480, point) == TouchFrameResult::Invalid);
    printf("Touch parser passed %u status/length/bounds checks plus null/release/dimension checks\n", checks);
}
