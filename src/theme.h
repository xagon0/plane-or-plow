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

// Scope furniture
static const RGB C_ROAD        = { 20,  33,  43 };
static const RGB C_ROAD_MAJOR  = { 28,  46,  60 };
static const RGB C_RING        = { 30,  54,  70 };
static const RGB C_RING_OUTER  = { 52,  92, 116 };
static const RGB C_TICK        = { 62, 104, 130 };
static const RGB C_TICK_NORTH  = {120, 180, 215 };

// Motion
static const RGB C_PULSE       = { 64, 148, 194 };

// Home
static const RGB C_HOME        = {190, 230, 255 };

// Contacts
static const RGB C_AIR_CORE    = {170, 226, 255 };
static const RGB C_AIR_GLOW    = { 46, 136, 224 };
static const RGB C_AIR_TRAIL   = { 58, 146, 226 };

static const RGB C_PLOW_CORE   = {255, 214, 158 };
static const RGB C_PLOW_GLOW   = {228, 138,  28 };
static const RGB C_PLOW_TRAIL  = {214, 128,  34 };

// Type
static const RGB C_TEXT        = {154, 192, 216 };
static const RGB C_TEXT_DIM    = { 68,  94, 114 };
static const RGB C_TEXT_FAINT  = { 44,  62,  78 };
static const RGB C_TEXT_AIR    = {126, 196, 244 };
static const RGB C_TEXT_PLOW   = {242, 174,  86 };
static const RGB C_TEXT_ALERT  = {226,  96,  84 };

// ---------------------------------------------------------------------------
// Scope geometry
// ---------------------------------------------------------------------------
#define SCOPE_CX      240
#define SCOPE_CY      240
#define SCOPE_R       218    // inscribed, leaving the corners for the readouts

#endif // THEME_H
