#ifndef AMBIENT_H
#define AMBIENT_H

#include <lvgl.h>

#define MAX_VEHICLES  48
#define MAX_TRAIL_PTS 20   // ~30 min at 90s/source poll rate

struct TrailPoint {
    float lat;
    float lon;
};

struct TrackedVehicle {
    char id[12];                      // aircraft hex ("A1B2C3") or plow itemId ("2263425")
    float dist_km;                    // most recent distance
    TrailPoint trail[MAX_TRAIL_PTS];  // ring buffer
    int trail_head;                   // index of newest point
    int trail_count;                  // valid points (0..MAX_TRAIL_PTS)
    uint8_t age;                      // polls since last seen (0 = just seen)
};

// Shared proximity state — written by network, read by ambient
struct ProximityState {
    bool airplane_nearby = false;
    bool snowplow_nearby = false;

    TrackedVehicle aircraft[MAX_VEHICLES];
    int aircraft_count = 0;

    TrackedVehicle plows[MAX_VEHICLES];
    int plow_count = 0;
};

extern ProximityState proximity;
extern float radius_km;   // runtime map radius, used by network + ambient

void ambient_init();
void ambient_cycle_radius();
void ambient_toggle_roads();

#endif // AMBIENT_H
