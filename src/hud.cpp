#include "hud.h"
#include "ambient.h"
#include "config.h"
#include "theme.h"
#include "timekeep.h"
#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The scope is a circle inscribed in a square panel, so the four corners and
// the top/bottom strips are the only places type can live without sitting on
// top of the map. The layout is built around exactly that.
// ---------------------------------------------------------------------------
static lv_obj_t *lbl_title    = NULL;
static lv_obj_t *lbl_air_n    = NULL;
static lv_obj_t *lbl_air_cap  = NULL;
static lv_obj_t *lbl_plow_n   = NULL;
static lv_obj_t *lbl_plow_cap = NULL;
static lv_obj_t *lbl_clock    = NULL;
static lv_obj_t *lbl_range    = NULL;
static lv_obj_t *lbl_nearest  = NULL;

static inline lv_color_t col(RGB c) { return lv_color_make(c.r, c.g, c.b); }

static lv_obj_t *make_label(const lv_font_t *font, RGB c, int letter_space,
                            lv_align_t align, int dx, int dy) {
    lv_obj_t *l = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col(c), 0);
    lv_obj_set_style_text_letter_space(l, letter_space, 0);
    lv_obj_align(l, align, dx, dy);
    lv_label_set_text(l, "");
    return l;
}

// 11300 -> "11,300"
static void format_grouped(int32_t v, char *out, size_t n) {
    char raw[16];
    snprintf(raw, sizeof(raw), "%ld", (long)v);
    int len = (int)strlen(raw);
    int o = 0;
    for (int i = 0; i < len && o < (int)n - 1; i++) {
        if (i > 0 && ((len - i) % 3 == 0)) out[o++] = ',';
        if (o < (int)n - 1) out[o++] = raw[i];
    }
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Nearest contact readout
// ---------------------------------------------------------------------------
static void update_nearest() {
    const TrackedVehicle *best = NULL;
    bool best_is_air = true;

    for (int i = 0; i < proximity.aircraft_count; i++) {
        const TrackedVehicle &v = proximity.aircraft[i];
        if (!best || v.dist_km < best->dist_km) { best = &v; best_is_air = true; }
    }
    for (int i = 0; i < proximity.plow_count; i++) {
        const TrackedVehicle &v = proximity.plows[i];
        if (!best || v.dist_km < best->dist_km) { best = &v; best_is_air = false; }
    }

    if (!best) {
        lv_label_set_text(lbl_nearest, "NO CONTACTS");
        lv_obj_set_style_text_color(lbl_nearest, col(C_TEXT_FAINT), 0);
        lv_obj_set_style_text_letter_space(lbl_nearest, 3, 0);
        return;
    }

    char line[64];
    if (best_is_air) {
        const char *name = (best->label[0] != '\0') ? best->label : best->id;
        if (best->alt_ft > 0) {
            char alt[16];
            format_grouped(best->alt_ft, alt, sizeof(alt));
            snprintf(line, sizeof(line), "%s   %.1f km   %s ft",
                     name, best->dist_km, alt);
        } else {
            snprintf(line, sizeof(line), "%s   %.1f km", name, best->dist_km);
        }
        lv_obj_set_style_text_color(lbl_nearest, col(C_TEXT_AIR), 0);
    } else {
        snprintf(line, sizeof(line), "SNOWPLOW   %.1f km", best->dist_km);
        lv_obj_set_style_text_color(lbl_nearest, col(C_TEXT_PLOW), 0);
    }
    lv_obj_set_style_text_letter_space(lbl_nearest, 1, 0);
    lv_label_set_text(lbl_nearest, line);
}

static void hud_tick_cb(lv_timer_t *timer) {
    (void)timer;

    char buf[12];

    snprintf(buf, sizeof(buf), "%d", proximity.aircraft_count);
    lv_label_set_text(lbl_air_n, buf);
    lv_obj_set_style_text_color(lbl_air_n,
        col(proximity.aircraft_count ? C_TEXT_AIR : C_TEXT_FAINT), 0);
    lv_obj_set_style_text_color(lbl_air_cap,
        col(proximity.aircraft_count ? C_TEXT_DIM : C_TEXT_FAINT), 0);

    snprintf(buf, sizeof(buf), "%d", proximity.plow_count);
    lv_label_set_text(lbl_plow_n, buf);
    lv_obj_set_style_text_color(lbl_plow_n,
        col(proximity.plow_count ? C_TEXT_PLOW : C_TEXT_FAINT), 0);
    lv_obj_set_style_text_color(lbl_plow_cap,
        col(proximity.plow_count ? C_TEXT_DIM : C_TEXT_FAINT), 0);

    // "--:--" means the clock has never synced, which is the one failure that
    // would otherwise be invisible: the schedule silently assumes daytime and
    // the panel just never dims.
    bool online = (WiFi.status() == WL_CONNECTED);
    struct tm t;
    if (time_local(&t)) {
        char clk[8];
        snprintf(clk, sizeof(clk), "%02d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text(lbl_clock, clk);
        lv_obj_set_style_text_color(lbl_clock,
            col(online ? C_TEXT_DIM : C_TEXT_PLOW), 0);
    } else {
        lv_label_set_text(lbl_clock, "--:--");
        lv_obj_set_style_text_color(lbl_clock, col(C_TEXT_ALERT), 0);
    }

    update_nearest();
}

// ---------------------------------------------------------------------------
void hud_init() {
    // Top strip — above the scope rim.
    lbl_title = make_label(&lv_font_montserrat_14, C_TEXT_FAINT, 4,
                           LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(lbl_title, "PLANE OR PLOW");

    // Upper corners — caption above the count. The caption is the wide part,
    // so it goes where the corner is widest; the narrow numeral drops into the
    // tighter space below without touching the scope rim.
    lbl_air_cap = make_label(&lv_font_montserrat_14, C_TEXT_DIM, 3,
                             LV_ALIGN_TOP_LEFT, 17, 20);
    lv_label_set_text(lbl_air_cap, "AIRCRAFT");
    lbl_air_n   = make_label(&lv_font_montserrat_24, C_TEXT_AIR, 0,
                             LV_ALIGN_TOP_LEFT, 16, 40);

    lbl_plow_cap = make_label(&lv_font_montserrat_14, C_TEXT_DIM, 3,
                              LV_ALIGN_TOP_RIGHT, -17, 20);
    lv_label_set_text(lbl_plow_cap, "PLOWS");
    lbl_plow_n   = make_label(&lv_font_montserrat_24, C_TEXT_PLOW, 0,
                              LV_ALIGN_TOP_RIGHT, -16, 40);

    // Lower corners — link state and scope range.
    // The clock replaces the old ONLINE label. Link state was already carried
    // by the home marker turning red, so that word was redundant — and a
    // schedule that depends on the clock ought to show the clock.
    lbl_clock = make_label(&lv_font_montserrat_20, C_TEXT_DIM, 1,
                           LV_ALIGN_BOTTOM_LEFT, 16, -26);
    lv_label_set_text(lbl_clock, "--:--");

    lbl_range = make_label(&lv_font_montserrat_14, C_TEXT_FAINT, 3,
                           LV_ALIGN_BOTTOM_RIGHT, -16, -30);
    {
        char r[16];
        snprintf(r, sizeof(r), "%d KM", (int)RADIUS_KM);
        lv_label_set_text(lbl_range, r);
    }

    // Bottom strip — the nearest contact.
    lbl_nearest = make_label(&lv_font_montserrat_16, C_TEXT_FAINT, 1,
                             LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_label_set_text(lbl_nearest, "NO CONTACTS");

    lv_timer_create(hud_tick_cb, 250, NULL);
}
