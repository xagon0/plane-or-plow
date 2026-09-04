#include "backlight.h"
#include "ambient.h"
#include "config.h"
#include "network.h"
#include "weather.h"
#include "timekeep.h"
#include "status.h"
#include "theme.h"
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

// Manual brightness, set by dragging the left edge. Holds until the next
// scheduled transition takes it back.
static bool manual_active = false;
static bool in_bright_drag = false;      // this gesture started in the strip
static lv_obj_t *bar_track = NULL;
static lv_obj_t *bar_fill  = NULL;
static uint32_t bar_hide_ms = 0;

static bool is_daytime() {
    struct tm t;
    if (!time_local(&t)) {
        // The clock has never been set. Staying lit is the safe default, but it
        // is also exactly the failure that looks like "the schedule is broken",
        // so the panel shows --:-- and /status says so rather than hiding it.
        return last_daytime;
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
// ---------------------------------------------------------------------------
// Manual brightness strip
// ---------------------------------------------------------------------------
static void show_bar(uint8_t duty) {
    if (!bar_track || !bar_fill) return;
    int span = BRIGHT_BOT_Y - BRIGHT_TOP_Y;
    int h = (span * (int)duty) / 255;
    if (h < 2) h = 2;
    lv_obj_set_pos(bar_fill, 10, BRIGHT_BOT_Y - h);
    lv_obj_set_size(bar_fill, 6, h);
    lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bar_fill,  LV_OBJ_FLAG_HIDDEN);
    bar_hide_ms = millis() + BRIGHT_HOLD_MS;
}

static void set_manual_from_y(int y) {
    if (y < BRIGHT_TOP_Y) y = BRIGHT_TOP_Y;
    if (y > BRIGHT_BOT_Y) y = BRIGHT_BOT_Y;
    int span = BRIGHT_BOT_Y - BRIGHT_TOP_Y;
    int duty = (255 * (BRIGHT_BOT_Y - y)) / span;
    if (duty < 0) duty = 0;
    if (duty > 255) duty = 255;

    // Track the finger with no easing: a 1.6 s fade under direct manipulation
    // feels like lag, not smoothness.
    target_duty = (uint8_t)duty;
    current_duty = (float)duty;
    ledcWrite(BL_PWM_CHANNEL, (uint8_t)duty);
    manual_active = true;
    peek_active = false;
    show_bar((uint8_t)duty);
}

static void tap_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t pt;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_get_point(indev, &pt);
    else { pt.x = pt.y = 0; }

    switch (code) {
        case LV_EVENT_PRESSED:
            in_bright_drag = (pt.x < BRIGHT_ZONE_W);
            if (in_bright_drag) {
                set_manual_from_y(pt.y);
                Serial.printf("Backlight: manual %d%% (touch %d,%d)\r\n",
                              (target_duty * 100) / 255, (int)pt.x, (int)pt.y);
            }
            break;

        case LV_EVENT_PRESSING:
            if (in_bright_drag) set_manual_from_y(pt.y);
            break;

        case LV_EVENT_RELEASED:
            if (in_bright_drag) {
                Serial.printf("Backlight: manual set to %d%%\r\n",
                              (target_duty * 100) / 255);
            }
            break;

        case LV_EVENT_CLICKED:
            // Only the taps that did not start in the strip do the old thing.
            if (!in_bright_drag) backlight_on_tap();
            in_bright_drag = false;
            break;

        default:
            break;
    }
}

static void create_tap_overlay() {
    lv_obj_t *tap_overlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(tap_overlay);
    lv_obj_set_size(tap_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(tap_overlay, 0, 0);
    lv_obj_set_style_bg_opa(tap_overlay, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(tap_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(tap_overlay, tap_event_cb, LV_EVENT_CLICKED,  NULL);

    // Level indicator, created after the overlay so it draws on top of it.
    bar_track = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(bar_track);
    lv_obj_set_pos(bar_track, 10, BRIGHT_TOP_Y);
    lv_obj_set_size(bar_track, 6, BRIGHT_BOT_Y - BRIGHT_TOP_Y);
    lv_obj_set_style_radius(bar_track, 3, 0);
    lv_obj_set_style_bg_color(bar_track, lv_color_make(C_TEXT_FAINT.r, C_TEXT_FAINT.g, C_TEXT_FAINT.b), 0);
    lv_obj_set_style_bg_opa(bar_track, LV_OPA_50, 0);
    lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar_track, LV_OBJ_FLAG_HIDDEN);

    bar_fill = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(bar_fill);
    lv_obj_set_pos(bar_fill, 10, BRIGHT_TOP_Y);
    lv_obj_set_size(bar_fill, 6, 10);
    lv_obj_set_style_radius(bar_fill, 3, 0);
    lv_obj_set_style_bg_color(bar_fill, lv_color_make(C_AIR_CORE.r, C_AIR_CORE.g, C_AIR_CORE.b), 0);
    lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(bar_fill, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void backlight_init() {
    ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RESOLUTION);
    ledcAttachPin(BL_PIN, BL_PWM_CHANNEL);

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
        // Replay the last hour of radar, and pull fresh contacts while we are
        // at it. The loop is the one thing a still radar frame cannot tell you:
        // whether the weather is arriving or leaving.
        weather_replay();
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
        weather_replay();
        network_boost_polling();
    }
}

uint8_t backlight_duty() { return (uint8_t)(current_duty + 0.5f); }
bool    backlight_is_daytime() { return last_daytime; }
bool    backlight_manual() { return manual_active; }

void backlight_update() {
    bool day = is_daytime();

    if (day != was_daytime) {
        struct tm t;
        bool have = time_local(&t);
        target_duty = day ? BL_DAY_DUTY : BL_NIGHT_DUTY;
        peek_active = false;
        uint32_t up = millis() / 1000UL;
        snprintf(stats.bl_last, sizeof(stats.bl_last),
                 "%s at %02d:%02d%s (uptime %luh %lum)",
                 day ? "night->day" : "day->night",
                 have ? t.tm_hour : 0, have ? t.tm_min : 0,
                 have ? "" : " CLOCK-UNSET",
                 (unsigned long)(up / 3600), (unsigned long)((up % 3600) / 60));
        stats.bl_transitions++;
        manual_active = false;      // the schedule takes control back
        Serial.printf("Backlight: %s\r\n", stats.bl_last);
    }
    was_daytime = day;

    if (bar_hide_ms && millis() >= bar_hide_ms) {
        bar_hide_ms = 0;
        if (bar_track) lv_obj_add_flag(bar_track, LV_OBJ_FLAG_HIDDEN);
        if (bar_fill)  lv_obj_add_flag(bar_fill,  LV_OBJ_FLAG_HIDDEN);
    }

    if (peek_active && millis() >= peek_deadline) {
        target_duty = BL_NIGHT_DUTY;
        peek_active = false;
    }

    fade_step();
}
