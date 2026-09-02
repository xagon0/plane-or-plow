#ifndef THEME_H
#define THEME_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Palette — a cold, cinematic night scope. All values are 8-bit RGB; the
// renderer packs to RGB565 at blend time.
//
// The scope is monochrome-cool by default so the two contact colours (cyan
// aircraft, amber plows) are the only warm/saturated things on screen.
// ---------------------------------------------------------------------------

struct RGB { uint8_t r, g, b; };

// Ground
static const RGB C_BG_CENTER   = { 11,  22,  31 };   // beneath the scope
static const RGB C_BG_EDGE     = {  3,   6,  10 };   // scope rim
static const RGB C_BG_CORNER   = {  2,   3,   5 };   // outside the scope

// Map geometry. Water is the only saturated hue on the basemap — roads read as
// a neutral luminance ladder instead, warm-grey at the top and cooling as they
// recede, so a river looks like a river and a highway looks like a road.
// Weight is carried by the per-class alpha in ambient.cpp, not by these values.
static const RGB C_MAP_WATER   = { 36, 104, 152 };   // Bow River, creeks, lakes
static const RGB C_MAP_MINOR   = { 96, 104, 112 };   // residential, range roads
static const RGB C_MAP_ROAD    = {124, 126, 126 };   // secondary, tertiary
static const RGB C_MAP_MAJOR   = {162, 158, 150 };   // highways, runways

// Scope furniture
static const RGB C_RING        = { 38,  66,  86 };
static const RGB C_RING_OUTER  = { 52,  92, 116 };
static const RGB C_TICK        = { 62, 104, 130 };
static const RGB C_TICK_NORTH  = {120, 180, 215 };

// Motion
static const RGB C_PULSE       = { 64, 148, 194 };

// Home
static const RGB C_HOME        = {190, 230, 255 };

// Contacts. Aircraft sit near hue 165: clear of the amber plows at 35, clear of
// the precipitation ramp's pink top end near 325, and clear of the Bow River at
// 205 — which the old aircraft cyan shared, separated only by brightness.
static const RGB C_AIR_CORE    = {170, 255, 227 };
static const RGB C_AIR_GLOW    = { 19, 251, 211 };
static const RGB C_AIR_TRAIL   = { 33, 251, 210 };

static const RGB C_PLOW_CORE   = {255, 214, 158 };
static const RGB C_PLOW_GLOW   = {228, 138,  28 };
static const RGB C_PLOW_TRAIL  = {214, 128,  34 };

// Precipitation. Cool for ordinary snow; only the top third heats, through a
// near-white pivot into amber, red and pink. Blue straight to orange would pass
// through brown, which reads as a rendering fault rather than as weather, and
// routing through white makes the warm end mean "heavier" rather than "other".
struct WxStop { float t; uint8_t r, g, b, a; };

static const WxStop C_WX_STOPS[] = {
    { 0.00f,  40,  56, 104,   0 },
    { 0.10f,  56,  74, 132,  22 },
    { 0.28f,  78, 112, 186,  44 },
    { 0.45f, 130, 168, 224,  65 },
    { 0.58f, 196, 214, 242,  82 },
    { 0.68f, 245, 236, 214,  96 },   // pivot
    { 0.78f, 250, 190, 110, 126 },
    { 0.88f, 240, 120,  80, 140 },
    { 0.95f, 232,  74,  96, 150 },
    { 1.00f, 236,  96, 168, 158 },
};
static const int C_WX_STOP_COUNT = sizeof(C_WX_STOPS) / sizeof(WxStop);

// Shade sunk under a contact so it survives a bright precipitation field.
static const RGB C_CASING      = {  5,   9,  14 };

// Inbound cue drawn on the rim
static const RGB C_INBOUND     = {150, 190, 240 };

// Type
static const RGB C_TEXT        = {154, 192, 216 };
static const RGB C_TEXT_DIM    = { 68,  94, 114 };
static const RGB C_TEXT_FAINT  = { 44,  62,  78 };
static const RGB C_TEXT_AIR    = {115, 255, 219 };
static const RGB C_TEXT_PLOW   = {242, 174,  86 };
static const RGB C_TEXT_ALERT  = {226,  96,  84 };

// ---------------------------------------------------------------------------
// Scope geometry
// ---------------------------------------------------------------------------
#define SCOPE_CX      240
#define SCOPE_CY      240
#define SCOPE_R       218    // inscribed, leaving the corners for the readouts

#endif // THEME_H
