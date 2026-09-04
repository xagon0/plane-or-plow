#ifndef CONFIG_H
#define CONFIG_H

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "src/secrets.h is missing. Create it with:  python3 tools/configure.py --address \"Your Town\" --ssid YourWiFi --password YourPassword   (see README.md)"
#endif

// WIFI_SSID / WIFI_PASS / HOME_NAME / HOME_LAT / HOME_LON / RADIUS_KM / HOME_TZ
// all come from secrets.h, which is gitignored. See secrets.h.example.

// --- API endpoints ---
// The aircraft query is built at runtime from HOME_LAT/HOME_LON so that moving
// the scope actually moves the query. Radius is appended in nautical miles, so
// the shape is  <host>/<lat>/<lon>/<nm>.
//
// adsb.lol serves this shape and asks only that you identify yourself.
// airplanes.live serves the same shape but has disabled public API access: it
// returns 403 to everything except a small set of allowlisted User-Agent
// strings, which is not a foundation to build on. adsb.fi is another option but
// uses a different path form (/api/v2/lat/<lat>/lon/<lon>/dist/<nm>), so it
// needs the URL builder in network.cpp adjusting rather than just this line.
#define AIRPLANE_API_HOST "https://api.adsb.lol/v2/point"
#define PLOW_API_URL      "https://511.alberta.ca/map/mapIcons/ServiceVehicles"

// --- HTTP identity ---
// Say who we are. If you fork this, put your own URL here.
#define HTTP_USER_AGENT \
    "plane-or-plow/1.0 (+https://github.com/xagon0/plane-or-plow)"

// --- Aircraft filtering ---
// ADS-B carries more than aircraft. In emitter category set C, C1 and C2 are
// surface vehicles (emergency and service) and C3-C5 are obstacles. C0 is NOT
// one of them — it means "no emitter category information", which real
// aircraft do report, so filtering it drops planes. Only C1-C7 are dropped.
#define FILTER_SURFACE_VEHICLES 1
// Aircraft parked or taxiing report alt_baro as the string "ground". They are
// static dots at an airport several km away, not something you can hear.
#define FILTER_ON_GROUND        1

// --- Timing ---
// Aircraft take 3 of every 4 ticks, plows the 4th: aircraft refresh ~27s,
// plows ~80s. Shorter aircraft gaps mean the renderer extrapolates less
// between fixes, which is what keeps their motion smooth.
#define API_POLL_MS       20000   // one source per tick
#define BOOST_POLL_MS     8000    // faster ticks after a tap
#define BOOST_DURATION_MS 180000  // 3 min of fast polling after interaction
#define FRAME_MS          33      // ~30 FPS

// --- Display ---
#define SCREEN_W 480
#define SCREEN_H 480

// --- Time ---
// SNTP is started only after WiFi is up; starting it before means the first
// resolution fails and lwIP's retry does not come round again for an hour.
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"
#define NTP_SERVER_3  "time.nist.gov"
#define NTP_RETRY_MS  120000UL     // re-kick a clock that has never synced

// --- Backlight schedule (MST, 24h) ---
#define SCHEDULE_ON_HOUR  7
#define SCHEDULE_ON_MIN   30
#define SCHEDULE_OFF_HOUR 23
#define SCHEDULE_OFF_MIN  0

// --- Backlight PWM ---
#define BL_PIN           38
#define BL_PWM_CHANNEL   7
#define BL_PWM_FREQ      300
#define BL_PWM_RESOLUTION 8

// Brightness is fully automatic now — no on-screen control. Transitions ease
// between these levels rather than stepping, so the schedule change reads as
// the room dimming rather than a switch being thrown.
#define BL_DAY_DUTY      255
#define BL_NIGHT_DUTY    0
#define BL_PEEK_DUTY     26      // ~10%, enough to read in a dark room
#define BL_FADE_MS       1600    // full-range fade time

// --- Weather radar (Environment Canada GeoMet, open data) ---
// RADAR_1KM_RSNO is the snow-rate product; the server publishes a new volume
// scan every 6 minutes, so there is no point asking more often than that.
#define WX_ENABLED        1
#define WX_HOST           "https://geo.weather.gc.ca/geomet"
#define WX_LAYER          "RADAR_1KM_RSNO"
#define WX_STYLE          "Radar-Snow_14colors"
#define WX_N              64        // grid cells across the scope box
#define WX_BOX_KM         16.5f     // half-width of the fetched bbox
#define WX_POLL_MS        360000UL  // 6 min, matching the scan cadence
#define WX_TICK_MS        20000     // how often we re-evaluate what to fetch
#define WX_READ_MS        9000      // hard ceiling on one tile read
#define WX_STALL_MS       2500      // give up if the stream goes quiet this long
#define WX_WIDE_POLL_MS   720000UL  // 12 min; the wide picture changes slowly
#define WX_HISTORY        10        // frames kept for the replay loop
#define WX_REPLAY_STEP_MS 320       // per frame during a replay

// Wide-area sweep: the scope only sees 30 km, which is about 35 minutes of
// warning. This second, coarser fetch answers "is anything heading here?"
#define WX_WIDE_N         32
#define WX_WIDE_BOX_KM    150.0f
#define WX_WIDE_MIN_KM    18.0f     // ignore echoes already inside the scope
#define WX_WIDE_THRESH    46        // 0..255 intensity worth reporting

// --- Manual brightness strip ---
// Drag or tap the left edge to set brightness directly, full at the top down
// to off at the bottom. This overrides the schedule until the next scheduled
// transition, which then takes it back — so a manual dim never becomes a
// permanently dark panel you have to remember to undo.
#define BRIGHT_ZONE_W   96      // left fifth of a 480 px panel
#define BRIGHT_TOP_Y    34      // y that means 100%
#define BRIGHT_BOT_Y    446     // y that means off
#define BRIGHT_HOLD_MS  1400    // indicator lingers this long after release

// --- Night peek ---
#define NIGHT_PEEK_MS    15000

#endif // CONFIG_H
