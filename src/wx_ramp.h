#ifndef WX_RAMP_H
#define WX_RAMP_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Environment Canada snow-rate colour ramp, sampled from the GeoMet legend for
// RADAR_1KM_RSNO / Radar-Snow_14colors. Weakest (0.1 cm/h) first, strongest
// (20 cm/h) last; the scale is logarithmic in rate, so a pixel's position along
// this ramp is already a perceptually sensible intensity.
//
// The server antialiases when it resamples to our bbox, so tiles come back with
// far more colours than the palette holds. Nearest-neighbour against this ramp
// is what turns an arbitrary pixel back into an intensity.
// ---------------------------------------------------------------------------

#define WX_RAMP_N 32

static const uint8_t wx_ramp[WX_RAMP_N][3] = {
    {140,199,254},   // 0.00
    { 64,174,254},   // 0.03
    {  0,152,254},   // 0.06
    {  0,203,178},   // 0.10
    {  0,254,102},   // 0.13
    {  0,231, 55},   // 0.16
    {  0,207,  8},   // 0.19
    {  0,184,  0},   // 0.23
    {  0,161,  0},   // 0.26
    {  0,138,  0},   // 0.29
    {  0,112,  0},   // 0.32
    { 64,140,  0},   // 0.35
    {180,210,  0},   // 0.39
    {254,246,  0},   // 0.42
    {254,222,  0},   // 0.45
    {254,199,  0},   // 0.48
    {254,174,  0},   // 0.52
    {254,152,  0},   // 0.55
    {254,127,  0},   // 0.58
    {254,104,  0},   // 0.61
    {254, 59,  0},   // 0.65
    {254, 13,  0},   // 0.68
    {254,  1, 57},   // 0.71
    {254,  2,127},   // 0.74
    {224, 16,167},   // 0.77
    {178, 39,191},   // 0.81
    {142, 40,193},   // 0.84
    {119, 17,169},   // 0.87
    { 93,  0,140},   // 0.90
    { 70,  0,105},   // 0.94
    { 51,  0, 77},   // 0.97
    { 51,  0, 77},   // 1.00
};

#endif // WX_RAMP_H
