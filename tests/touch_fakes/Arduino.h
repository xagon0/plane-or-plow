#pragma once
#include <stdint.h>
#include <stddef.h>

extern uint32_t fake_millis;
inline uint32_t millis() { return fake_millis; }
struct FakeSerial {
    void println(const char *) {}
    void printf(const char *, ...) {}
};
extern FakeSerial Serial;
