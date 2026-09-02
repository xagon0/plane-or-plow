#include "ambient.h"
#include "config.h"
#include "network.h"
#include "roads_data.h"
#include "theme.h"
#include "gfx_draw.h"
#include "hud.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
ProximityState proximity;
float radius_km = RADIUS_KM;

#define DEG2RAD (M_PI / 180.0f)

// ---------------------------------------------------------------------------
// Buffers: a static background rendered once, memcpy'd in as the ground for
// every frame. Redrawing the road network at 30 FPS was most of the old
// frame cost; now it is paid once at boot.
// ---------------------------------------------------------------------------
static lv_obj_t *canvas   = NULL;
static uint16_t *canvas_buf = NULL;
static uint16_t *bg_buf     = NULL;

static float pixels_per_km;
static float lon_scale;

static uint32_t boot_ms = 0;

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------
static inline void latlon_to_screen(float lat, float lon, float &sx, float &sy) {
    float dy_km = (lat - (float)HOME_LAT) * 111.32f;
    float dx_km = (lon - (float)HOME_LON) * 111.32f * lon_scale;
    sx = (float)SCOPE_CX + dx_km * pixels_per_km;
    sy = (float)SCOPE_CY - dy_km * pixels_per_km;   // north = up
}

// Where a contact is *now*, extrapolated from its last fix along its track.
static void projected_latlon(const TrackedVehicle &v, float &lat, float &lon) {
    lat = v.lat;
    lon = v.lon;
    if (v.speed_kts <= 1.0f || v.heading_deg < 0.0f) return;

    uint32_t dt = millis() - v.fix_ms;
    if (dt > 150000) dt = 150000;          // never run away if polling stalls
    float km = v.speed_kts * 1.852f / 3600.0f * ((float)dt / 1000.0f);
    float h  = v.heading_deg * DEG2RAD;
    lat += km * cosf(h) / 111.32f;
    lon += km * sinf(h) / (111.32f * lon_scale);
}

void trail_push(TrackedVehicle &v, float lat, float lon) {
    if (v.trail_count == 0) {
        v.trail_head = 0;
    } else {
        v.trail_head = (uint8_t)((v.trail_head + 1) % TRAIL_PTS);
    }
    v.trail[v.trail_head].lat = lat;
    v.trail[v.trail_head].lon = lon;
    if (v.trail_count < TRAIL_PTS) v.trail_count++;
    v.trail_ms = millis();
}

// ---------------------------------------------------------------------------
// Static background
// ---------------------------------------------------------------------------
// 8x8 ordered dither. The background ramp spans about five distinct RGB565
// levels across the whole radius, so quantising it straight would draw
// concentric banding rings. Dithering the quantisation hides them.
static const uint8_t bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
};

static inline int dither_channel(float v255, float bits_max, float d) {
    int q = (int)(v255 * bits_max / 255.0f + d + 0.5f);
    if (q < 0) q = 0;
    if (q > (int)bits_max) q = (int)bits_max;
    return q;
}

static void draw_background_gradient() {
    gfx_mask_enable(false);
    for (int y = 0; y < SCREEN_H; y++) {
        uint16_t *row = &bg_buf[y * SCREEN_W];
        float dy = (float)y + 0.5f - (float)SCOPE_CY;
        float dy2 = dy * dy;
        for (int x = 0; x < SCREEN_W; x++) {
            float dx = (float)x + 0.5f - (float)SCOPE_CX;
            float d  = sqrtf(dx * dx + dy2);
            float fr, fg, fb;
            if (d >= (float)SCOPE_R) {
                fr = C_BG_CORNER.r; fg = C_BG_CORNER.g; fb = C_BG_CORNER.b;
            } else {
                // Falls off faster than linear so the centre reads as a
                // pool of light rather than a flat wash.
                float t = powf(d / (float)SCOPE_R, 0.72f);
                fr = C_BG_CENTER.r + (C_BG_EDGE.r - C_BG_CENTER.r) * t;
                fg = C_BG_CENTER.g + (C_BG_EDGE.g - C_BG_CENTER.g) * t;
                fb = C_BG_CENTER.b + (C_BG_EDGE.b - C_BG_CENTER.b) * t;
            }
            float dth = (float)bayer8[((y & 7) << 3) | (x & 7)] / 64.0f - 0.5f;
            row[x] = (uint16_t)((dither_channel(fr, 31.0f, dth) << 11) |
                                (dither_channel(fg, 63.0f, dth) <<  5) |
                                 dither_channel(fb, 31.0f, dth));
        }
    }
    gfx_mask_enable(true);
}

static void draw_map() {
    // Indexed by MAP_WATER / MAP_MINOR / MAP_ROAD / MAP_MAJOR. Ways are stored
    // in that order, so this also fixes the painter's order: water underneath,
    // highways on top.
    const RGB   cls_col[4] = { C_MAP_WATER, C_MAP_MINOR, C_MAP_ROAD, C_MAP_MAJOR };
    const float cls_w[4]   = { 1.7f,        1.0f,        1.4f,       1.9f        };
    // Alpha, not colour, sets how much of the map you notice. Residential sits
    // at ~10% so the towns read as texture rather than as drawn lines.
    const uint8_t cls_a[4] = { 165,         26,          40,         62          };

    for (int w = 0; w < MAP_WAY_COUNT; w++) {
        const MapWay &way = map_ways[w];
        if (way.cls > 3) continue;
        const MapPt *pts = &map_pts[way.start];
        const RGB col = cls_col[way.cls];
        const float wid = cls_w[way.cls];
        const uint8_t alpha = cls_a[way.cls];

        float x0, y0;
        latlon_to_screen(pts[0].lat, pts[0].lon, x0, y0);
        for (int i = 1; i < (int)way.count; i++) {
            float x1, y1;
            latlon_to_screen(pts[i].lat, pts[i].lon, x1, y1);
            gfx_stroke(bg_buf, x0, y0, x1, y1, col, alpha, wid);
            x0 = x1; y0 = y1;
        }
    }
}

static void draw_scope_furniture() {
    const float cx = (float)SCOPE_CX, cy = (float)SCOPE_CY;

    // Range rings at 1/3 and 2/3 of the scope.
    gfx_ring(bg_buf, cx, cy, SCOPE_R * 0.3333f, C_RING, 195, 1.0f);
    gfx_ring(bg_buf, cx, cy, SCOPE_R * 0.6667f, C_RING, 180, 1.0f);

    // Minor bearing ticks every 15 degrees, just inside the rim.
    for (int i = 0; i < 24; i++) {
        float a = (float)i * 15.0f * DEG2RAD;
        float s = sinf(a), c = cosf(a);
        bool cardinal = (i % 6 == 0);
        if (cardinal) continue;
        float inner = (float)SCOPE_R - ((i % 2 == 0) ? 9.0f : 5.0f);
        gfx_line(bg_buf, cx + s * inner, cy - c * inner,
                         cx + s * (SCOPE_R - 1.0f), cy - c * (SCOPE_R - 1.0f),
                 C_TICK, 110);
    }

    // Cardinal ticks, north emphasised.
    for (int i = 0; i < 4; i++) {
        float a = (float)i * 90.0f * DEG2RAD;
        float s = sinf(a), c = cosf(a);
        bool north = (i == 0);
        RGB col = north ? C_TICK_NORTH : C_TICK;
        float inner = (float)SCOPE_R - (north ? 20.0f : 15.0f);
        gfx_stroke(bg_buf, cx + s * inner, cy - c * inner,
                           cx + s * (SCOPE_R - 1.0f), cy - c * (SCOPE_R - 1.0f),
                   col, north ? 235 : 175, north ? 2.2f : 1.6f);
    }

    // The rim: a bright hairline over a dim halo, which also anti-aliases the
    // hard edge of the round mask.
    gfx_ring(bg_buf, cx, cy, (float)SCOPE_R - 3.0f, C_RING,       70,  3.0f);
    gfx_ring(bg_buf, cx, cy, (float)SCOPE_R - 1.0f, C_RING_OUTER, 210, 1.4f);
}

// ---------------------------------------------------------------------------
// Sonar pulses — two expanding rings, phase offset. This replaces the old
// rotating-sweep idea deliberately: a pulse radiating from home is calmer,
// reads correctly as "range", and costs one ring per frame instead of a
// per-pixel wedge.
// ---------------------------------------------------------------------------
#define PULSE_PERIOD_MS 5200.0f
#define PULSE_COUNT     2

static float pulse_prev_r[PULSE_COUNT] = {0};

static void draw_pulses(uint32_t now, float *pulse_r_out) {
    for (int i = 0; i < PULSE_COUNT; i++) {
        float phase = (float)i / (float)PULSE_COUNT;
        float t = fmodf((float)(now - boot_ms) / PULSE_PERIOD_MS + phase, 1.0f);
        float r = t * (float)(SCOPE_R - 2);
        pulse_r_out[i] = r;

        // Fade in over the first sliver, then decay outward.
        float fade_in = (t < 0.06f) ? (t / 0.06f) : 1.0f;
        float a = powf(1.0f - t, 1.7f) * fade_in * 96.0f;
        if (a < 1.0f) continue;

        gfx_ring(canvas_buf, (float)SCOPE_CX, (float)SCOPE_CY, r,
                 C_PULSE, (uint8_t)a, 2.0f);
        // A wider, dimmer skirt gives the ring some body.
        gfx_ring(canvas_buf, (float)SCOPE_CX, (float)SCOPE_CY, r,
                 C_PULSE, (uint8_t)(a * 0.3f), 5.0f);
    }
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------
#define AGE_FADE_POLLS 5.0f

static void draw_trail(const TrackedVehicle &v, RGB col, float alpha_scale) {
    if (v.trail_count < 2) return;

    int oldest = (v.trail_head - v.trail_count + 1 + TRAIL_PTS) % TRAIL_PTS;
    float px = 0, py = 0;
    for (int s = 0; s < v.trail_count; s++) {
        int idx = (oldest + s) % TRAIL_PTS;
        float x, y;
        latlon_to_screen(v.trail[idx].lat, v.trail[idx].lon, x, y);
        if (s > 0) {
            float t = (float)s / (float)(v.trail_count - 1);   // 0 old -> 1 new
            // Comet tail: alpha and width both taper toward the head.
            uint8_t a = (uint8_t)(powf(t, 1.6f) * 150.0f * alpha_scale);
            float   w = 0.9f + 1.5f * t;
            gfx_stroke(canvas_buf, px, py, x, y, col, a, w);
        }
        px = x; py = y;
    }
}

// Aircraft: a triangle pointing along track. Plows: a small diamond.
static void draw_glyph(float x, float y, float heading_deg, bool is_air,
                       RGB col, uint8_t alpha) {
    float xs[4], ys[4];
    int n;

    if (is_air) {
        static const float lx[3] = { 0.0f, -4.1f,  4.1f };
        static const float ly[3] = { -6.6f, 4.9f,  4.9f };
        n = 3;
        float h = (heading_deg < 0.0f) ? 0.0f : heading_deg * DEG2RAD;
        float ch = cosf(h), sh = sinf(h);
        for (int i = 0; i < 3; i++) {
            xs[i] = x + lx[i] * ch - ly[i] * sh;
            ys[i] = y + lx[i] * sh + ly[i] * ch;
        }
    } else {
        static const float lx[4] = { 0.0f, 4.2f, 0.0f, -4.2f };
        static const float ly[4] = { -4.8f, 0.0f, 4.8f, 0.0f };
        n = 4;
        for (int i = 0; i < 4; i++) { xs[i] = x + lx[i]; ys[i] = y + ly[i]; }
    }
    gfx_poly(canvas_buf, xs, ys, n, col, alpha);
}

static void draw_contacts(TrackedVehicle *list, int count, bool is_air,
                          uint32_t now, const float *pulse_r, float dt_s) {
    RGB core  = is_air ? C_AIR_CORE  : C_PLOW_CORE;
    RGB glow  = is_air ? C_AIR_GLOW  : C_PLOW_GLOW;
    RGB trail = is_air ? C_AIR_TRAIL : C_PLOW_TRAIL;

    for (int i = 0; i < count; i++) {
        TrackedVehicle &v = list[i];

        float lat, lon;
        projected_latlon(v, lat, lon);

        // Aircraft sample their own trail from the interpolated position, so
        // the tail is smooth instead of a 90-second staircase.
        if (is_air && (now - v.trail_ms) >= AIR_TRAIL_SAMPLE_MS) {
            trail_push(v, lat, lon);
        }

        float x, y;
        latlon_to_screen(lat, lon, x, y);

        // Fade contacts out as they go stale rather than popping them off.
        float age_f = 1.0f - ((float)v.age / AGE_FADE_POLLS);
        if (age_f < 0.0f) age_f = 0.0f;
        age_f = 0.25f + 0.75f * age_f;

        draw_trail(v, trail, age_f);

        float dx = x - (float)SCOPE_CX, dy = y - (float)SCOPE_CY;
        float r_px = sqrtf(dx * dx + dy * dy);
        if (r_px > (float)SCOPE_R - 2.0f) {
            v.flash = 0.0f;      // don't hold a stale ping while off-scope
            continue;
        }

        // Closer to home = brighter.
        float t = r_px / (float)SCOPE_R;
        float bright = (0.45f + 0.55f * (1.0f - t) * (1.0f - t)) * age_f;

        // Ping when a sonar pulse sweeps past this contact's range.
        for (int p = 0; p < PULSE_COUNT; p++) {
            if (pulse_prev_r[p] < r_px && pulse_r[p] >= r_px) v.flash = 1.0f;
        }
        if (v.flash > 0.0f) {
            float f = v.flash;
            gfx_ring(canvas_buf, x, y, 7.0f + (1.0f - f) * 15.0f,
                     core, (uint8_t)(f * f * 130.0f), 1.4f);
            v.flash -= dt_s / 0.65f;
            if (v.flash < 0.0f) v.flash = 0.0f;
        }

        gfx_glow(canvas_buf, x, y, 14, glow, (uint8_t)(bright * 150.0f));
        gfx_glow(canvas_buf, x, y, 6,  core, (uint8_t)(bright * 110.0f));

        float heading = v.heading_deg;
        if (heading < 0.0f && v.trail_count >= 2) {
            // No track from the API — infer it from the last two trail points.
            int h0 = v.trail_head;
            int h1 = (h0 - 1 + TRAIL_PTS) % TRAIL_PTS;
            float ax, ay, bx, by;
            latlon_to_screen(v.trail[h1].lat, v.trail[h1].lon, ax, ay);
            latlon_to_screen(v.trail[h0].lat, v.trail[h0].lon, bx, by);
            if (fabsf(bx - ax) > 0.5f || fabsf(by - ay) > 0.5f) {
                heading = atan2f(bx - ax, ay - by) / DEG2RAD;
            }
        }
        draw_glyph(x, y, heading, is_air, core, (uint8_t)(bright * 255.0f));
    }
}

// ---------------------------------------------------------------------------
// Home marker
// ---------------------------------------------------------------------------
static void draw_home(uint32_t now, bool wifi_ok) {
    const float cx = (float)SCOPE_CX, cy = (float)SCOPE_CY;

    // ~4.4 s breath.
    float br = (sinf((float)(now - boot_ms) * 2.0f * (float)M_PI / 4400.0f) + 1.0f) * 0.5f;
    RGB col = wifi_ok ? C_HOME : C_TEXT_ALERT;

    gfx_glow(canvas_buf, cx, cy, 16, col, (uint8_t)(26.0f + br * 34.0f));
    gfx_ring(canvas_buf, cx, cy, 6.5f + br * 1.2f, col,
             (uint8_t)(90.0f + br * 70.0f), 1.3f);

    float xs[4] = { cx, cx + 2.1f, cx, cx - 2.1f };
    float ys[4] = { cy - 2.1f, cy, cy + 2.1f, cy };
    gfx_poly(canvas_buf, xs, ys, 4, col, 255);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------
static uint32_t last_frame_ms = 0;

static void render_frame() {
    if (!canvas_buf || !bg_buf) return;

    uint32_t now = millis();
    float dt_s = (last_frame_ms == 0) ? 0.033f : (float)(now - last_frame_ms) / 1000.0f;
    if (dt_s > 0.5f) dt_s = 0.5f;
    last_frame_ms = now;

    memcpy(canvas_buf, bg_buf, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));

    float pulse_r[PULSE_COUNT];
    draw_pulses(now, pulse_r);

    draw_contacts(proximity.plows,    proximity.plow_count,     false, now, pulse_r, dt_s);
    draw_contacts(proximity.aircraft, proximity.aircraft_count, true,  now, pulse_r, dt_s);

    draw_home(now, WiFi.status() == WL_CONNECTED);

    for (int p = 0; p < PULSE_COUNT; p++) pulse_prev_r[p] = pulse_r[p];

    lv_obj_invalidate(canvas);
}

static void frame_tick_cb(lv_timer_t *timer) {
    (void)timer;
    render_frame();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void ambient_init() {
    boot_ms = millis();
    radius_km    = (float)RADIUS_KM;
    pixels_per_km = (float)SCOPE_R / radius_km;
    lon_scale     = cosf((float)HOME_LAT * DEG2RAD);

    gfx_init();

    size_t buf_bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t);
    canvas_buf = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    bg_buf     = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!canvas_buf || !bg_buf) {
        Serial.println("FATAL: canvas PSRAM alloc failed!");
        return;
    }

    uint32_t t0 = millis();
    draw_background_gradient();
    draw_map();
    draw_scope_furniture();
    memcpy(canvas_buf, bg_buf, buf_bytes);
    Serial.printf("Background composited in %lu ms\r\n", (unsigned long)(millis() - t0));

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, canvas_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, SCREEN_W, SCREEN_H);

    hud_init();

    lv_timer_create(frame_tick_cb, FRAME_MS, NULL);

    Serial.printf("Scope ready: %.1f px/km, range %.0f km\r\n",
                  pixels_per_km, radius_km);
}
