#include "touch_input.h"
#include "diagnostics.h"
#include <Arduino.h>
#include <Wire.h>
#include <algorithm>
#include <initializer_list>

namespace {
constexpr uint16_t POINT_STATUS = 0x814e;
constexpr uint16_t FIRST_XY = 0x8150;
constexpr uint32_t MAX_RETRY_MS = 5000;
constexpr uint32_t RELEASE_WITHOUT_FRAME_MS = 100;
TouchStats stats;
uint16_t screen_width = 480, screen_height = 480;
int pin_sda = -1, pin_scl = -1;
bool bus_ready = false;
bool pressed = false;
bool retry_pending = false;
bool controller_logged = false;
uint32_t failed_ms = 0, retry_ms = 250, last_frame_ms = 0;
TouchPoint last_point = {};

bool read_register(uint8_t address, uint16_t reg, uint8_t *out, size_t size) {
    const uint8_t register_bytes[] = {uint8_t(reg >> 8), uint8_t(reg)};
    Wire.beginTransmission(address);
    if (Wire.write(register_bytes, sizeof(register_bytes)) != sizeof(register_bytes)) {
        Wire.endTransmission(true);
        return false;
    }
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(address, size, true) != size) return false;
    for (size_t i = 0; i < size; ++i) {
        const int value = Wire.read();
        if (value < 0) return false;
        out[i] = uint8_t(value);
    }
    return true;
}

bool acknowledge_frame() {
    const uint8_t bytes[] = {uint8_t(POINT_STATUS >> 8), uint8_t(POINT_STATUS), 0};
    Wire.beginTransmission(stats.address);
    const bool written = Wire.write(bytes, sizeof(bytes)) == sizeof(bytes);
    const uint8_t result = Wire.endTransmission(true);
    return written && result == 0;
}

void fail(bool invalid_frame = false) {
    pressed = false;
    stats.available = false;
    ++stats.errors;
    if (invalid_frame) ++stats.invalid_frames;
    failed_ms = millis();
    if (retry_pending) retry_ms = std::min(retry_ms * 2, MAX_RETRY_MS);
    else retry_ms = 250;
    retry_pending = true;
    if (stats.errors == 1) {
        Serial.println("Touch: I2C/frame error; input released, retrying with backoff");
    }
}

void mark_healthy() {
    if (retry_pending) {
        ++stats.recoveries;
        Serial.printf("Touch: recovered at 0x%02x after %lu errors\r\n",
                      stats.address, (unsigned long)stats.errors);
    }
    retry_pending = false;
    retry_ms = 250;
}

bool discover() {
    if (!bus_ready) {
        bus_ready = Wire.begin(pin_sda, pin_scl, 100000);
        Wire.setTimeOut(20);
        if (!bus_ready) return false;
    }
    for (uint8_t address : {uint8_t(0x5d), uint8_t(0x14)}) {
        Wire.beginTransmission(address);
        if (Wire.endTransmission(true) != 0) continue;
        // Reading product ID validates a responding touch controller without
        // resetting unavailable pins or rewriting its factory configuration.
        uint8_t product[4] = {};
        if (!read_register(address, 0x8140, product, sizeof(product))) continue;
        if (product[0] != '9' || product[1] != '1' || product[2] != '1') continue;
        stats.address = address;
        stats.available = true;
        if (!controller_logged) {
            Serial.printf("Touch: GT911 ready at 0x%02x (checked reads, 20ms timeout)\r\n", address);
            controller_logged = true;
        }
        return true;
    }
    return false;
}
}

const TouchStats &touch_input_stats() { return stats; }

void touch_input_init(int sda, int scl, uint16_t width, uint16_t height) {
    pin_sda = sda;
    pin_scl = scl;
    screen_width = width;
    screen_height = height;
    if (!discover()) fail();
}

bool touch_input_read(TouchPoint &point) {
    DiagnosticsPhase phase("touch-read");
    point = last_point;
    if (retry_pending && uint32_t(millis() - failed_ms) < retry_ms) return false;
    if (!stats.available && !discover()) {
        fail();
        return false;
    }

    uint8_t status = 0;
    if (!read_register(stats.address, POINT_STATUS, &status, 1)) {
        fail();
        return false;
    }
    if (!(status & 0x80)) {
        mark_healthy();
        // No new controller frame. Retain a held contact briefly, but never
        // let a missing release packet leave LVGL pressed indefinitely.
        if (uint32_t(millis() - last_frame_ms) >= RELEASE_WITHOUT_FRAME_MS) pressed = false;
        return pressed;
    }

    uint8_t xy[4] = {};
    const uint8_t count = status & 0x0f;
    if (count >= 1 && count <= 5 && !read_register(stats.address, FIRST_XY, xy, sizeof(xy))) {
        fail();
        return false;
    }
    const TouchFrameResult result = parse_touch_frame(status, xy, sizeof(xy),
                                                      screen_width, screen_height, point);
    // Clear the ready flag even for a malformed frame so it cannot remain
    // latched. An acknowledgement error also releases input immediately.
    if (!acknowledge_frame()) {
        fail();
        return false;
    }
    if (result == TouchFrameResult::Invalid) {
        fail(true);
        return false;
    }
    ++stats.frames;
    mark_healthy();
    last_frame_ms = millis();
    pressed = result == TouchFrameResult::Pressed;
    if (pressed) last_point = point;
    return pressed;
}
