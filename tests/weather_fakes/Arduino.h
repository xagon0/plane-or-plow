#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t fake_weather_millis;
inline uint32_t millis() { return fake_weather_millis; }
struct WeatherFakeSerial {
    void println(const char *) {}
    void printf(const char *, ...) {}
};
extern WeatherFakeSerial Serial;
struct WeatherFakeESP { unsigned getFreeHeap() const { return 200000; } };
extern WeatherFakeESP ESP;
inline unsigned uxTaskGetStackHighWaterMark(void *) { return 10000; }
inline size_t weather_fake_strlcpy(char *dst, const char *src, size_t size) {
    const size_t length = strlen(src);
    if (size) {
        const size_t count = length < size - 1 ? length : size - 1;
        memcpy(dst, src, count);
        dst[count] = 0;
    }
    return length;
}
#define strlcpy weather_fake_strlcpy
