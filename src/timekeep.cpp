#include "timekeep.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>

// A clock that has never been set reads as 1970. Anything past this is real.
#define TIME_SANE_EPOCH 1700000000L

static bool     started = false;
static bool     ever_synced = false;
static uint32_t last_attempt_ms = 0;
static uint32_t last_sync_ms = 0;

static void start_sntp() {
    // Three servers, so one unreachable pool entry is not the whole story.
    configTzTime(HOME_TZ, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    last_attempt_ms = millis();
    started = true;
}

void time_init() {
    start_sntp();
    Serial.printf("NTP: started, TZ=%s, servers %s / %s / %s\r\n",
                  HOME_TZ, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
}

bool time_valid() {
    time_t now;
    time(&now);
    return now > TIME_SANE_EPOCH;
}

bool time_local(struct tm *out) {
    time_t now;
    time(&now);
    if (now <= TIME_SANE_EPOCH) return false;
    localtime_r(&now, out);
    return true;
}

uint32_t time_since_sync_s() {
    if (!ever_synced) return 0;
    return (millis() - last_sync_ms) / 1000UL;
}

void time_tick() {
    if (!started) {
        if (WiFi.status() == WL_CONNECTED) time_init();
        return;
    }

    if (time_valid()) {
        if (!ever_synced) {
            ever_synced = true;
            last_sync_ms = millis();
            struct tm t;
            if (time_local(&t)) {
                Serial.printf("NTP: synced, local time %04d-%02d-%02d %02d:%02d\r\n",
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                              t.tm_hour, t.tm_min);
            }
        }
        return;
    }

    // Never synced. SNTP's own retry is slow enough that a boot which raced
    // WiFi could sit wrong for an hour, so re-kick it every couple of minutes
    // until it lands.
    if (WiFi.status() == WL_CONNECTED &&
        (millis() - last_attempt_ms) > NTP_RETRY_MS) {
        Serial.println("NTP: no time yet, retrying");
        start_sntp();
    }
}
