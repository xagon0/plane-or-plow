#include "backlight.h"
#include "ambient.h"
#include "config.h"
#include "network.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Brightness table — full range at 150 Hz (community-tested for this board)
// ---------------------------------------------------------------------------
static const uint8_t bright_table[] = {255, 180, 120, 70, 35, 10, 0};
static const int BRIGHT_STEPS = sizeof(bright_table) / sizeof(bright_table[0]);

static int bright_idx = 0;          // current index into bright_table
static bool was_daytime = true;     // edge detection for schedule transitions

// Night peek state
static bool peek_active = false;
static unsigned long peek_deadline = 0;

// ---------------------------------------------------------------------------
// PWM helpers
// ---------------------------------------------------------------------------
static void set_brightness(uint8_t duty) {
    ledcWrite(BL_PWM_CHANNEL, duty);
}

// ---------------------------------------------------------------------------
// NTP / schedule
// ---------------------------------------------------------------------------
static bool ntp_started = false;

static void ntp_init() {
    configTzTime("MST7", "pool.ntp.org");
    ntp_started = true;
    Serial.println("NTP: configTzTime MST7");
}

static bool last_daytime = true;  // last known result (default daytime until NTP syncs)

static bool is_daytime() {
    struct tm t;
    if (!getLocalTime(&t, 0)) {
        return last_daytime;  // NTP glitch — return last known state, not "true"
    }
    int mins = t.tm_hour * 60 + t.tm_min;
    int on_mins  = SCHEDULE_ON_HOUR * 60 + SCHEDULE_ON_MIN;   // 7:30 = 450
    int off_mins = SCHEDULE_OFF_HOUR * 60 + SCHEDULE_OFF_MIN; // 23:00 = 1380
    last_daytime = (mins >= on_mins && mins < off_mins);
    return last_daytime;
}

// ---------------------------------------------------------------------------
// LVGL tap overlay — invisible full-screen clickable object
// ---------------------------------------------------------------------------
static lv_obj_t *tap_overlay = NULL;

static void tap_event_cb(lv_event_t *e) {
    (void)e;
    lv_point_t pt;
    lv_indev_get_point(lv_indev_get_act(), &pt);

    if (pt.x < SCREEN_W / 3 && pt.y < SCREEN_H / 3) {
        // Upper-left 1/3 zone → toggle roads overlay
        ambient_toggle_roads();
    } else if (pt.x < SCREEN_W / 3 && pt.y > SCREEN_H * 2 / 3) {
        // Lower-left 1/3 zone → radius cycling
        ambient_cycle_radius();
    } else {
        backlight_on_tap();
    }
}

static void create_tap_overlay() {
    tap_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(tap_overlay);
    lv_obj_set_size(tap_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(tap_overlay, 0, 0);
    lv_obj_set_style_bg_opa(tap_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tap_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_CLICKED, NULL);
    Serial.println("Backlight: tap overlay created");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void backlight_init() {
    // Set up LEDC PWM (ESP32 Arduino core 2.x API)
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
    ledcAttachPin(BL_PIN, BL_PWM_CHANNEL);

    // Start NTP
    ntp_init();

    // Initial state: 100% brightness
    bright_idx = 0;
    was_daytime = true;
    set_brightness(bright_table[bright_idx]);
    Serial.printf("Backlight: PWM init, duty=%d\r\n", bright_table[bright_idx]);

    // Create tap overlay on top of canvas (must be called after ambient_init)
    create_tap_overlay();
}

void backlight_on_tap() {
    bool day = is_daytime();

    if (day) {
        // Cycle through brightness steps, wrap to 100%
        bright_idx = (bright_idx + 1) % BRIGHT_STEPS;
        set_brightness(bright_table[bright_idx]);
        Serial.printf("Backlight: tap (day) → step %d, duty=%d\r\n",
                      bright_idx, bright_table[bright_idx]);
        if (bright_table[bright_idx] > 0) network_boost_polling();
    } else {
        // Night mode
        if (bright_table[bright_idx] > 0 || peek_active) {
            // Screen is on (or peeking) — turn off immediately
            bright_idx = BRIGHT_STEPS - 1;  // Off
            set_brightness(0);
            peek_active = false;
            Serial.println("Backlight: tap (night, on) → off");
        } else {
            // Screen is off — peek at 5% for 15s
            bright_idx = BRIGHT_STEPS - 2;  // 5% = duty 13
            set_brightness(bright_table[bright_idx]);
            peek_active = true;
            peek_deadline = millis() + NIGHT_PEEK_MS;
            Serial.println("Backlight: tap (night, off) → peek 5%");
            network_boost_polling();
        }
    }
}

void backlight_update() {
    bool day = is_daytime();

    // Edge detection: schedule transitions
    if (day && !was_daytime) {
        // Night → Day: turn on at 100%
        bright_idx = 0;
        set_brightness(bright_table[bright_idx]);
        peek_active = false;
        Serial.println("Backlight: night→day, 100%");
    } else if (!day && was_daytime) {
        // Day → Night: turn off
        bright_idx = BRIGHT_STEPS - 1;
        set_brightness(0);
        peek_active = false;
        Serial.println("Backlight: day→night, off");
    }
    was_daytime = day;

    // Night peek auto-off
    if (peek_active && millis() >= peek_deadline) {
        bright_idx = BRIGHT_STEPS - 1;
        set_brightness(0);
        peek_active = false;
        Serial.println("Backlight: peek expired, off");
    }
}
