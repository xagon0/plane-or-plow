#ifndef TIMEKEEP_H
#define TIMEKEEP_H

#include <stdint.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Wall-clock time.
//
// The backlight schedule depends entirely on this, and when it silently fails
// the symptom is a screen that never dims — with nothing on the display saying
// why. So the clock is now shown on the panel and reported by /status, and an
// unsynced clock reads as "--:--" rather than quietly pretending to be daytime.
// ---------------------------------------------------------------------------

// Start SNTP. Must be called AFTER WiFi is up: starting it earlier means the
// first resolution fails and the retry does not come round for an hour.
void time_init();

// Re-kick a clock that has never synced. Cheap; call from loop().
void time_tick();

bool time_valid();                       // has the clock ever been set
bool time_local(struct tm *out);         // local time, false if never synced
uint32_t time_since_sync_s();            // 0 if never synced

#endif // TIMEKEEP_H
