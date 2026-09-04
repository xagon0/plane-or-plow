#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Counters the subsystems feed, served as JSON on /status.
//
// The point of this is unattended diagnosis: leave the device running for days
// and one HTTP request answers whether heap is leaking, fragmenting, or fine,
// and whether any subsystem is quietly failing its fetches.
// ---------------------------------------------------------------------------
struct SysStats {
    uint32_t radar_fetches   = 0;
    uint32_t radar_failures  = 0;
    uint32_t radar_last_ms   = 0;
    uint32_t radar_last_bytes = 0;
    uint32_t air_ok    = 0, air_fail  = 0;
    uint32_t air_filtered = 0;   // ground/surface contacts dropped last poll
    uint32_t plow_ok   = 0, plow_fail = 0;
    uint32_t wifi_reconnects = 0;
    // Backlight schedule history, so "did it dim last night?" is answerable
    // the next morning from /status alone, without a serial cable.
    uint32_t bl_transitions  = 0;
    char     bl_last[56]     = "none since boot";

    uint32_t heap_at_boot    = 0;   // very early in setup()
    uint32_t heap_settled    = 0;   // once everything is initialised
    const char *reset_reason = "?";
};

extern SysStats stats;

// Records why the last run ended and the boot heap. Call early in setup().
void status_boot_report();

// Starts the HTTP server and mDNS. Call after WiFi is up.
void status_init();

// Records the heap baseline once startup allocation is done. Drift is measured
// from here, not from boot — otherwise normal init reads as a 50 KB "leak".
void status_mark_settled();

// Pump the server; call from loop().
void status_update();

#endif // STATUS_H
