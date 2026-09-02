#include "gfx_draw.h"
#include "config.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Round-mask span table: for each row, the inclusive x range inside the scope.
// Rows outside the circle get an empty span (x0 > x1).
// ---------------------------------------------------------------------------
static int16_t span_x0[SCREEN_H];
static int16_t span_x1[SCREEN_H];
static bool    mask_on = true;

// Gaussian falloff, indexed by (d/r)^2 * 63.
#define GLOW_LUT_N 64
static uint8_t glow_lut[GLOW_LUT_N];

void gfx_init() {
    for (int y = 0; y < SCREEN_H; y++) {
        float dy = (float)y + 0.5f - (float)SCOPE_CY;
        float inner = (float)(SCOPE_R * SCOPE_R) - dy * dy;
        if (inner <= 0.0f) {
            span_x0[y] = 1;
            span_x1[y] = 0;            // empty
            continue;
        }
        float half = sqrtf(inner);
        int x0 = (int)ceilf((float)SCOPE_CX - half);
        int x1 = (int)floorf((float)SCOPE_CX + half);
        if (x0 < 0) x0 = 0;
        if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
        span_x0[y] = (int16_t)x0;
        span_x1[y] = (int16_t)x1;
    }

    for (int i = 0; i < GLOW_LUT_N; i++) {
        float t = (float)i / (float)(GLOW_LUT_N - 1);   // (d/r)^2
        glow_lut[i] = (uint8_t)(expf(-3.2f * t) * 255.0f);
    }
}

void gfx_mask_enable(bool on) { mask_on = on; }

// ---------------------------------------------------------------------------
// Pixel ops
// ---------------------------------------------------------------------------
static inline bool visible(int x, int y) {
    if (y < 0 || y >= SCREEN_H) return false;
    if (mask_on) return (x >= span_x0[y] && x <= span_x1[y]);
    return (x >= 0 && x < SCREEN_W);
}

void gfx_blend(uint16_t *buf, int x, int y, RGB c, uint8_t a) {
    if (a == 0 || !visible(x, y)) return;
    uint16_t *p = &buf[y * SCREEN_W + x];
    uint16_t d = *p;
    int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    int sr = c.r >> 3,          sg = c.g >> 2,        sb = c.b >> 3;
    dr += ((sr - dr) * a) >> 8;
    dg += ((sg - dg) * a) >> 8;
    db += ((sb - db) * a) >> 8;
    *p = (uint16_t)((dr << 11) | (dg << 5) | db);
}

void gfx_add(uint16_t *buf, int x, int y, RGB c, uint8_t a) {
    if (a == 0 || !visible(x, y)) return;
    uint16_t *p = &buf[y * SCREEN_W + x];
    uint16_t d = *p;
    int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    dr += ((c.r >> 3) * a) >> 8;
    dg += ((c.g >> 2) * a) >> 8;
    db += ((c.b >> 3) * a) >> 8;
    if (dr > 31) dr = 31;
    if (dg > 63) dg = 63;
    if (db > 31) db = 31;
    *p = (uint16_t)((dr << 11) | (dg << 5) | db);
}

// ---------------------------------------------------------------------------
// Wu anti-aliased line
// ---------------------------------------------------------------------------
static inline float fpart(float x) { return x - floorf(x); }

void gfx_line(uint16_t *buf, float x0, float y0, float x1, float y1,
              RGB c, uint8_t a) {
    if (a == 0) return;

    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    if (steep) { float t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }

    float dx = x1 - x0;
    float dy = y1 - y0;
    float grad = (dx == 0.0f) ? 1.0f : dy / dx;

    // Cheap reject: a segment far outside the canvas costs nothing.
    float lo = (y0 < y1) ? y0 : y1, hi = (y0 < y1) ? y1 : y0;
    int limit = steep ? SCREEN_W : SCREEN_H;
    if (x1 < -2.0f || x0 > (float)(steep ? SCREEN_H : SCREEN_W) + 2.0f) return;
    if (hi < -2.0f || lo > (float)limit + 2.0f) return;

    int ix0 = (int)floorf(x0 + 0.5f);
    int ix1 = (int)floorf(x1 + 0.5f);
    float inter = y0 + grad * ((float)ix0 + 0.5f - x0);

    for (int x = ix0; x <= ix1; x++, inter += grad) {
        int   yi = (int)floorf(inter);
        float fy = inter - (float)yi;
        uint8_t a0 = (uint8_t)((1.0f - fy) * (float)a);
        uint8_t a1 = (uint8_t)(fy * (float)a);
        if (steep) {
            gfx_blend(buf, yi,     x, c, a0);
            gfx_blend(buf, yi + 1, x, c, a1);
        } else {
            gfx_blend(buf, x, yi,     c, a0);
            gfx_blend(buf, x, yi + 1, c, a1);
        }
    }
}

// ---------------------------------------------------------------------------
// Soft stroke — parallel AA lines offset along the segment normal.
// ---------------------------------------------------------------------------
void gfx_stroke(uint16_t *buf, float x0, float y0, float x1, float y1,
                RGB c, uint8_t a, float width) {
    if (width <= 1.05f) { gfx_line(buf, x0, y0, x1, y1, c, a); return; }

    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len, ny = dx / len;

    int n = (int)ceilf(width);
    if (n > 5) n = 5;
    float step = width / (float)n;
    float start = -(width - step) * 0.5f;

    for (int i = 0; i < n; i++) {
        float off = start + step * (float)i;
        // Feather the outer passes so the stroke has soft shoulders.
        float w = 1.0f - 0.45f * fabsf(off) / (width * 0.5f + 0.001f);
        uint8_t aa = (uint8_t)((float)a * w);
        gfx_line(buf, x0 + nx * off, y0 + ny * off,
                      x1 + nx * off, y1 + ny * off, c, aa);
    }
}

// ---------------------------------------------------------------------------
// Ring as an AA polyline
// ---------------------------------------------------------------------------
void gfx_ring(uint16_t *buf, float cx, float cy, float radius,
              RGB c, uint8_t a, float width) {
    if (radius < 0.5f || a == 0) return;
    int segs = (int)(radius * 1.1f);
    if (segs < 16)  segs = 16;
    if (segs > 220) segs = 220;

    float step = 2.0f * (float)M_PI / (float)segs;
    float px = cx + radius, py = cy;
    for (int i = 1; i <= segs; i++) {
        float ang = step * (float)i;
        float nx2 = cx + radius * cosf(ang);
        float ny2 = cy + radius * sinf(ang);
        gfx_stroke(buf, px, py, nx2, ny2, c, a, width);
        px = nx2; py = ny2;
    }
}

// ---------------------------------------------------------------------------
// Convex polygon fill, 3x3 supersampled
// ---------------------------------------------------------------------------
void gfx_poly(uint16_t *buf, const float *xs, const float *ys, int n,
              RGB c, uint8_t a) {
    if (n < 3 || n > 4 || a == 0) return;

    float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x1 < 0 || y1 < 0 || x0 >= SCREEN_W || y0 >= SCREEN_H) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W - 1) x1 = SCREEN_W - 1;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++) {
                float py = (float)y + ((float)sy + 0.5f) / 3.0f;
                for (int sx = 0; sx < 3; sx++) {
                    float px = (float)x + ((float)sx + 0.5f) / 3.0f;
                    bool pos = false, neg = false;
                    for (int i = 0; i < n; i++) {
                        int j = (i + 1) % n;
                        float cr = (xs[j] - xs[i]) * (py - ys[i]) -
                                   (ys[j] - ys[i]) * (px - xs[i]);
                        if (cr > 0.0f) pos = true;
                        else if (cr < 0.0f) neg = true;
                        if (pos && neg) break;
                    }
                    if (!(pos && neg)) hits++;
                }
            }
            if (hits) gfx_blend(buf, x, y, c, (uint8_t)((a * hits) / 9));
        }
    }
}

// ---------------------------------------------------------------------------
// Additive gaussian glow
// ---------------------------------------------------------------------------
void gfx_glow(uint16_t *buf, float x, float y, int radius, RGB c, uint8_t a) {
    if (radius < 1 || a == 0) return;
    int cxi = (int)floorf(x + 0.5f);
    int cyi = (int)floorf(y + 0.5f);
    float inv_r2 = 1.0f / (float)(radius * radius);

    for (int py = cyi - radius; py <= cyi + radius; py++) {
        if (py < 0 || py >= SCREEN_H) continue;
        float dy = (float)py - y;
        float dy2 = dy * dy;
        for (int px = cxi - radius; px <= cxi + radius; px++) {
            float dx = (float)px - x;
            float d2 = dx * dx + dy2;
            if (d2 >= (float)(radius * radius)) continue;
            int idx = (int)(d2 * inv_r2 * (float)(GLOW_LUT_N - 1));
            if (idx < 0) idx = 0;
            if (idx > GLOW_LUT_N - 1) idx = GLOW_LUT_N - 1;
            uint8_t g = (uint8_t)((glow_lut[idx] * a) >> 8);
            gfx_add(buf, px, py, c, g);
        }
    }
}
