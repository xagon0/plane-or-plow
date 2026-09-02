#include "ambient.h"
#include "config.h"
#include "network.h"
#include "roads_data.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
ProximityState proximity;

// ---------------------------------------------------------------------------
// Runtime radius
// ---------------------------------------------------------------------------
static const float radius_table[] = {10, 15, 25, 50, 100};
static const int RADIUS_STEPS = sizeof(radius_table) / sizeof(radius_table[0]);
static int radius_idx = 1;          // start at 15km (matches original default)
float radius_km = 15.0f;

static lv_obj_t *radius_label = NULL;

static void update_radius_label() {
    if (!radius_label) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%dkm", (int)radius_km);
    lv_label_set_text(radius_label, buf);
}

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------
static inline uint16_t packRGB565(uint8_t r8, uint8_t g8, uint8_t b8) {
    return ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3);
}

// ---------------------------------------------------------------------------
// Map projection constants (computed once at init)
// ---------------------------------------------------------------------------
static float pixels_per_km;
static float lon_scale;  // cos(HOME_LAT) for longitude→km correction

// ---------------------------------------------------------------------------
// Dot glow LUT
// ---------------------------------------------------------------------------
#define DOT_RADIUS 5
#define DOT_LUT_SIZE (DOT_RADIUS * DOT_RADIUS + 1)
static uint8_t dot_lut[DOT_LUT_SIZE];

static void build_dot_lut() {
    float sigma2 = (float)(DOT_RADIUS * DOT_RADIUS) / 3.0f;
    for (int i = 0; i < DOT_LUT_SIZE; i++) {
        float val = expf(-(float)i / sigma2);
        dot_lut[i] = (uint8_t)(val * 255.0f);
    }
}

// ---------------------------------------------------------------------------
// Map helpers
// ---------------------------------------------------------------------------
static inline void latlon_to_screen(float lat, float lon, int &sx, int &sy) {
    float dy_km = (lat - (float)HOME_LAT) * 111.32f;
    float dx_km = (lon - (float)HOME_LON) * 111.32f * lon_scale;
    sx = SCREEN_W / 2 + (int)(dx_km * pixels_per_km);
    sy = SCREEN_H / 2 - (int)(dy_km * pixels_per_km);  // north = up
}

// Closer = brighter (quadratic falloff)
static inline float brightness_from_dist(float dist_km) {
    float t = dist_km / radius_km;
    if (t > 1.0f) t = 1.0f;
    float b = 1.0f - t;
    return 0.10f + 0.90f * b * b;
}

// Stamp a small glowing dot (additive blend)
static void stamp_dot(uint16_t *buf, int px, int py,
                       float cr, float cg, float cb, float intensity) {
    int rad = DOT_RADIUS;
    int rad_sq = rad * rad;

    int r5 = (int)(cr * 31.0f * intensity);
    int g6 = (int)(cg * 63.0f * intensity);
    int b5 = (int)(cb * 31.0f * intensity);

    int y_start = (py - rad > 0) ? py - rad : 0;
    int y_end   = (py + rad < SCREEN_H - 1) ? py + rad : SCREEN_H - 1;
    int x_start = (px - rad > 0) ? px - rad : 0;
    int x_end   = (px + rad < SCREEN_W - 1) ? px + rad : SCREEN_W - 1;

    for (int y = y_start; y <= y_end; y++) {
        int dy = y - py;
        int dy2 = dy * dy;
        uint16_t *row = &buf[y * SCREEN_W];
        for (int x = x_start; x <= x_end; x++) {
            int dx = x - px;
            int dist2 = dx * dx + dy2;
            if (dist2 >= rad_sq) continue;

            int lut_idx = (dist2 < DOT_LUT_SIZE) ? dist2 : DOT_LUT_SIZE - 1;
            int alpha = dot_lut[lut_idx];
            if (alpha < 4) continue;

            uint16_t existing = row[x];
            int er = (existing >> 11) & 0x1F;
            int eg = (existing >> 5)  & 0x3F;
            int eb = existing & 0x1F;

            int nr = er + ((r5 * alpha) >> 8);
            int ng = eg + ((g6 * alpha) >> 8);
            int nb = eb + ((b5 * alpha) >> 8);

            if (nr > 31) nr = 31;
            if (ng > 63) ng = 63;
            if (nb > 31) nb = 31;

            row[x] = (nr << 11) | (ng << 5) | nb;
        }
    }
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
static lv_obj_t *canvas = NULL;
static uint16_t *canvas_buf = NULL;
static uint32_t frame_count = 0;

// ---------------------------------------------------------------------------
// Road overlay
// ---------------------------------------------------------------------------
static bool show_roads = true;

// Cohen-Sutherland line clipping
enum { CS_INSIDE = 0, CS_LEFT = 1, CS_RIGHT = 2, CS_BOTTOM = 4, CS_TOP = 8 };

static inline int cs_outcode(int x, int y) {
    int code = CS_INSIDE;
    if (x < 0)          code |= CS_LEFT;
    else if (x >= SCREEN_W) code |= CS_RIGHT;
    if (y < 0)          code |= CS_TOP;
    else if (y >= SCREEN_H) code |= CS_BOTTOM;
    return code;
}

static bool clip_line(int &x0, int &y0, int &x1, int &y1) {
    int oc0 = cs_outcode(x0, y0);
    int oc1 = cs_outcode(x1, y1);
    while (true) {
        if (!(oc0 | oc1)) return true;   // both inside
        if (oc0 & oc1)    return false;   // trivially outside
        int oc = oc0 ? oc0 : oc1;
        int x, y;
        if (oc & CS_TOP) {
            x = x0 + (int)((long)(x1 - x0) * (0 - y0) / (y1 - y0));
            y = 0;
        } else if (oc & CS_BOTTOM) {
            x = x0 + (int)((long)(x1 - x0) * (SCREEN_H - 1 - y0) / (y1 - y0));
            y = SCREEN_H - 1;
        } else if (oc & CS_RIGHT) {
            y = y0 + (int)((long)(y1 - y0) * (SCREEN_W - 1 - x0) / (x1 - x0));
            x = SCREEN_W - 1;
        } else { // CS_LEFT
            y = y0 + (int)((long)(y1 - y0) * (0 - x0) / (x1 - x0));
            x = 0;
        }
        if (oc == oc0) { x0 = x; y0 = y; oc0 = cs_outcode(x0, y0); }
        else            { x1 = x; y1 = y; oc1 = cs_outcode(x1, y1); }
    }
}

static void draw_line(uint16_t *buf, int x0, int y0, int x1, int y1, uint16_t color) {
    if (!clip_line(x0, y0, x1, y1)) return;
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        buf[y0 * SCREEN_W + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_roads() {
    uint16_t color = packRGB565(12, 20, 12);  // dark gray-green
    for (int r = 0; r < ROAD_COUNT; r++) {
        const RoadPt *pts = roads[r].points;
        int count = roads[r].count;
        for (int i = 0; i < count - 1; i++) {
            int x0, y0, x1, y1;
            latlon_to_screen(pts[i].lat, pts[i].lon, x0, y0);
            latlon_to_screen(pts[i + 1].lat, pts[i + 1].lon, x1, y1);
            draw_line(canvas_buf, x0, y0, x1, y1, color);
        }
    }
}

// ---------------------------------------------------------------------------
// Trail rendering
// ---------------------------------------------------------------------------
static void draw_trail(const TrackedVehicle &v, float base_r, float base_g, float base_b) {
    if (v.trail_count < 2) return;

    // Walk ring buffer from oldest to newest
    // Oldest index: (trail_head - trail_count + 1 + MAX_TRAIL_PTS) % MAX_TRAIL_PTS
    int oldest = (v.trail_head - v.trail_count + 1 + MAX_TRAIL_PTS) % MAX_TRAIL_PTS;

    for (int s = 0; s < v.trail_count - 1; s++) {
        int idx0 = (oldest + s) % MAX_TRAIL_PTS;
        int idx1 = (oldest + s + 1) % MAX_TRAIL_PTS;

        int x0, y0, x1, y1;
        latlon_to_screen(v.trail[idx0].lat, v.trail[idx0].lon, x0, y0);
        latlon_to_screen(v.trail[idx1].lat, v.trail[idx1].lon, x1, y1);

        // Brightness ramp: oldest segment = 0.08, newest segment = 0.50
        float t = (float)(s + 1) / (float)(v.trail_count - 1);  // 0..1
        float bright = 0.08f + 0.42f * t;

        uint8_t r8 = (uint8_t)(base_r * bright * 255.0f);
        uint8_t g8 = (uint8_t)(base_g * bright * 255.0f);
        uint8_t b8 = (uint8_t)(base_b * bright * 255.0f);

        draw_line(canvas_buf, x0, y0, x1, y1, packRGB565(r8, g8, b8));
    }
}

// ---------------------------------------------------------------------------
// Render one frame
// ---------------------------------------------------------------------------
static void render_frame() {
    if (!canvas_buf) return;

    // Black background
    memset(canvas_buf, 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));

    // Road overlay (behind everything)
    if (show_roads) draw_roads();

    // Draw aircraft trails (blue)
    for (int i = 0; i < proximity.aircraft_count; i++) {
        draw_trail(proximity.aircraft[i], 0.3f, 0.5f, 1.0f);
    }

    // Draw snowplow trails (amber)
    for (int i = 0; i < proximity.plow_count; i++) {
        draw_trail(proximity.plows[i], 1.0f, 0.6f, 0.1f);
    }

    // Pulsing home crosshair at center (~4s cycle)
    // White = WiFi connected, Red = disconnected
    int cx = SCREEN_W / 2;
    int cy = SCREEN_H / 2;
    float pulse = (sinf((float)frame_count * 2.0f * M_PI / (4.0f * (1000.0f / FRAME_MS))) + 1.0f) * 0.5f;
    uint8_t hb = 5 + (uint8_t)(pulse * 30.0f);
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    uint16_t home_color = wifi_ok ? packRGB565(hb, hb, hb) : packRGB565(hb, 0, 0);
    for (int d = -3; d <= 3; d++) {
        canvas_buf[cy * SCREEN_W + (cx + d)] = home_color;
        canvas_buf[(cy + d) * SCREEN_W + cx] = home_color;
    }

    // Draw aircraft dots (blue) — position from trail head
    for (int i = 0; i < proximity.aircraft_count; i++) {
        const TrackedVehicle &v = proximity.aircraft[i];
        const TrailPoint &head = v.trail[v.trail_head];
        int sx, sy;
        latlon_to_screen(head.lat, head.lon, sx, sy);
        if (sx < 0 || sx >= SCREEN_W || sy < 0 || sy >= SCREEN_H) continue;
        float bright = brightness_from_dist(v.dist_km);
        stamp_dot(canvas_buf, sx, sy, 0.3f, 0.5f, 1.0f, bright);
    }

    // Draw snowplow dots (amber) — position from trail head
    for (int i = 0; i < proximity.plow_count; i++) {
        const TrackedVehicle &v = proximity.plows[i];
        const TrailPoint &head = v.trail[v.trail_head];
        int sx, sy;
        latlon_to_screen(head.lat, head.lon, sx, sy);
        if (sx < 0 || sx >= SCREEN_W || sy < 0 || sy >= SCREEN_H) continue;
        float bright = brightness_from_dist(v.dist_km);
        stamp_dot(canvas_buf, sx, sy, 1.0f, 0.6f, 0.1f, bright);
    }

    frame_count++;
    lv_obj_invalidate(canvas);
}

// ---------------------------------------------------------------------------
// LVGL timer callback (~30 FPS)
// ---------------------------------------------------------------------------
static void frame_tick_cb(lv_timer_t *timer) {
    (void)timer;
    render_frame();
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------
void ambient_init() {
    // Compute map projection constants
    pixels_per_km = (float)(SCREEN_W / 2) / radius_km;
    lon_scale = cosf((float)HOME_LAT * M_PI / 180.0f);

    // Build dot glow LUT
    build_dot_lut();

    // Allocate canvas buffer in PSRAM (480 * 480 * 2 = 460800 bytes)
    size_t buf_bytes = SCREEN_W * SCREEN_H * sizeof(uint16_t);
    canvas_buf = (uint16_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("FATAL: Canvas PSRAM alloc failed!");
        return;
    }
    memset(canvas_buf, 0, buf_bytes);
    Serial.printf("Canvas buffer: %u bytes in PSRAM\r\n", buf_bytes);

    // Create LVGL canvas filling the screen
    lv_obj_t *scr = lv_scr_act();
    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, canvas_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_set_size(canvas, SCREEN_W, SCREEN_H);

    // Start frame timer
    lv_timer_create(frame_tick_cb, FRAME_MS, NULL);

    // Radius label in lower-left corner
    radius_label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(radius_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(radius_label, lv_color_make(100, 100, 100), 0);
    lv_obj_align(radius_label, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    update_radius_label();

    Serial.printf("Ambient map display initialized (%.1f px/km, lon_scale=%.4f)\r\n",
                  pixels_per_km, lon_scale);
}

// ---------------------------------------------------------------------------
// Radius cycling (called from backlight tap overlay on lower-left tap)
// ---------------------------------------------------------------------------
void ambient_cycle_radius() {
    radius_idx = (radius_idx + 1) % RADIUS_STEPS;
    radius_km = radius_table[radius_idx];
    pixels_per_km = (float)(SCREEN_W / 2) / radius_km;
    update_radius_label();
    Serial.printf("Radius: %dkm (%.1f px/km)\r\n", (int)radius_km, pixels_per_km);
    network_boost_polling();
}

// ---------------------------------------------------------------------------
// Road overlay toggle (called from backlight tap on upper-left zone)
// ---------------------------------------------------------------------------
void ambient_toggle_roads() {
    show_roads = !show_roads;
    Serial.printf("Roads overlay: %s\r\n", show_roads ? "ON" : "OFF");
    network_boost_polling();
}
