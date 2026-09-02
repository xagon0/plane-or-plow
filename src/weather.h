#ifndef WEATHER_H
#define WEATHER_H

#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Environment Canada radar.
//
// The feed is a volume scan every 6 minutes, not a stream, so "live" here means
// the newest published frame. Motion is available on demand instead: recent
// frames are kept and replayed on a tap.
// ---------------------------------------------------------------------------

// Intensity 0..255 over the scope box, row-major, north-up.
extern uint8_t *weather_grid;
extern bool     weather_valid;

// Wide-area sweep result: precipitation approaching from beyond the scope.
extern bool  weather_inbound;
extern float weather_inbound_bearing;   // degrees, 0 = north, clockwise
extern float weather_inbound_km;

void weather_init();
void weather_replay();        // play the stored history, then settle back
bool weather_replaying();

#endif // WEATHER_H
