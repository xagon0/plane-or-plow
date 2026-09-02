#ifndef GFX_DRAW_H
#define GFX_DRAW_H

#include <stdint.h>
#include "theme.h"

// ---------------------------------------------------------------------------
// Anti-aliased RGB565 software rasteriser.
//
// Everything is clipped to the scope circle via a precomputed per-row span
// table, so the round mask costs two compares per pixel instead of a sqrt.
// Call gfx_mask_enable(false) to paint the corners (background only).
// ---------------------------------------------------------------------------

void gfx_init();
void gfx_mask_enable(bool on);

// alpha / intensity arguments are 0..255 unless noted.
void gfx_blend(uint16_t *buf, int x, int y, RGB c, uint8_t a);
void gfx_add(uint16_t *buf, int x, int y, RGB c, uint8_t a);

// Wu-style anti-aliased line, alpha blended.
void gfx_line(uint16_t *buf, float x0, float y0, float x1, float y1,
              RGB c, uint8_t a);

// Soft stroke: `width` parallel AA lines, centre brightest.
void gfx_stroke(uint16_t *buf, float x0, float y0, float x1, float y1,
                RGB c, uint8_t a, float width);

// Circle drawn as an AA polyline; segment count scales with radius.
void gfx_ring(uint16_t *buf, float cx, float cy, float radius,
              RGB c, uint8_t a, float width);

// Convex polygon (3 or 4 vertices), 3x3 supersampled edges.
void gfx_poly(uint16_t *buf, const float *xs, const float *ys, int n,
              RGB c, uint8_t a);

// Additive gaussian glow, radius in pixels.
void gfx_glow(uint16_t *buf, float x, float y, int radius, RGB c, uint8_t a);

#endif // GFX_DRAW_H
