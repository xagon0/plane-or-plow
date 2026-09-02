#include "backlight.h"
#include "ambient.h"
#include "config.h"
#include "network.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Brightness is scheduled, not controlled. v1 cycled a 7-step table on every
// tap; that control is gone. What remains is the schedule, a night peek, and
// an eased transition between levels.
// ---------------------------------------------------------------------------
static float   current_duty = 0.0f;      // what the panel is showing
static uint8_t target_duty  = BL_DAY_DUTY;
static uint32_t last_fade_ms = 0;

static bool was_daytime  = true;
static bool last_daytime = true;         // sticky across NTP hiccups

static bool peek_active = false;
static unsigned long peek_deadline = 0;

static void ntp_init() {
    configTzTime("MST7", "pool.ntp.org");
    Serial.println("NTP: configTzTime MST7");
}

static bool is_daytime() {
    struct tm t;
    if (!getLocalTime(&t, 0)) {
        return last_daytime;  // NTP glitch — hold the last known state
    }
    int mins = t.tm_hour * 60 + t.tm_min;
    int on_mins  = SCHEDULE_ON_HOUR * 60 + SCHEDULE_ON_MIN;
    int off_mins = SCHEDULE_OFF_HOUR * 60 + SCHEDULE_OFF_MIN;
    last_daytime = (mins >= on_mins && mins < off_mins);
    return last_daytime;
}

// ---------------------------------------------------------------------------
// Eased fade toward the target, stepped from loop()
// ---------------------------------------------------------------------------
static void fade_step() {
    uint32_t now = millis();
    if (last_fade_ms == 0) last_fade_ms = now;
    uint32_t dt = now - last_fade_ms;
    if (dt == 0) return;
    last_fade_ms = now;

    float target = (float)target_duty;
    if (current_duty == target) return;

    float max_step = 255.0f * (float)dt / (float)BL_FADE_MS;
    float delta = target - current_duty;

    // Ease out: approach the target asymptotically, but never slower than a
    // floor step so the fade always finishes.
    float step = delta * 0.12f;
    if (fabsf(step) < max_step * 0.15f) step = (delta > 0 ? 1.0f : -1.0f) * max_step * 0.15f;
    if (fabsf(step) > max_step) step = (delta > 0 ? 1.0f : -1.0f) * max_step;

    if (fabsf(delta) <= fabsf(step)) current_duty = target;
    else current_duty += step;

    ledcWrite(BL_PWM_CHANNEL, (uint8_t)(current_duty + 0.5f));
}

// ---------------------------------------------------------------------------
// Tap: refresh data by day, peek at the scope by night.
// ---------------------------------------------------------------------------
static void tap_event_cb(lv_event_t *e) {
    (void)e;
    backlight_on_tap();
}

static void create_tap_overlay() {
    lv_obj_t *tap_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(tap_overlay);
    lv_obj_set_size(tap_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(tap_overlay, 0, 0);
    lv_obj_set_style_bg_opa(tap_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tap_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_CLICKED, NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void backlight_init() {
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
    ledcAttachPin(BL_PIN, BL_PWM_CHANNEL);

    ntp_init();

    // Fade up from black on boot rather than snapping on.
    current_duty = 0.0f;
    target_duty  = BL_DAY_DUTY;
    was_daytime  = true;
    last_fade_ms = millis();
    ledcWrite(BL_PWM_CHANNEL, 0);

    create_tap_overlay();
    Serial.println("Backlight: scheduled, fading up");
}

void backlight_on_tap() {
    if (is_daytime()) {
        network_boost_polling();
        return;
    }

    if (peek_active || target_duty > 0) {
        target_duty = BL_NIGHT_DUTY;
        peek_active = false;
    } else {
        target_duty = BL_PEEK_DUTY;
        peek_active = true;
        peek_deadline = millis() + NIGHT_PEEK_MS;
        network_boost_polling();
    }
}

void backlight_update() {
    bool day = is_daytime();

    if (day && !was_daytime) {
        target_duty = BL_DAY_DUTY;
        peek_active = false;
        Serial.println("Backlight: night -> day");
    } else if (!day && was_daytime) {
        target_duty = BL_NIGHT_DUTY;
        peek_active = false;
        Serial.println("Backlight: day -> night");
    }
    was_daytime = day;

    if (peek_active && millis() >= peek_deadline) {
        target_duty = BL_NIGHT_DUTY;
        peek_active = false;
    }

    fade_step();
}
