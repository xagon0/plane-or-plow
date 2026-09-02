#ifndef AMBIENT_H
#define AMBIENT_H

#include <lvgl.h>

#define MAX_VEHICLES  24
#define TRAIL_PTS     24

// Aircraft positions are dead-reckoned between fixes and sampled into the
// trail on this cadence. Plows have no velocity from the API, so they push a
// trail point per fix instead — which at a 90s poll gives ~36 min of history.
#define AIR_TRAIL_SAMPLE_MS 1000

struct TrailPoint {
    float lat;
    float lon;
};

struct TrackedVehicle {
    char  id[10];              // aircraft hex ("A1B2C3") or plow itemId
    char  label[10];           // callsign / flight number, may be empty

    float lat, lon;            // last reported fix
    float heading_deg;         // 0 = north, clockwise; <0 = unknown
    float speed_kts;           // ground speed; 0 = unknown
    int32_t alt_ft;            // barometric altitude; <0 = unknown/ground
    float dist_km;             // distance from home at last fix
    uint32_t fix_ms;           // millis() when the fix landed

    uint8_t age;               // polls since last seen (0 = just seen)

    // --- render-side state ---
    TrailPoint trail[TRAIL_PTS];
    uint8_t trail_head;
    uint8_t trail_count;
    uint32_t trail_ms;         // last trail sample time
    float flash;               // 1 -> 0 decay when the sonar pulse crosses it
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
extern float radius_km;    // scope range, fixed at RADIUS_KM

// Push a position onto a vehicle's trail ring buffer.
void trail_push(TrackedVehicle &v, float lat, float lon);

void ambient_init();

#endif // AMBIENT_H
