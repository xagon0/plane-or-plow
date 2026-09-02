#ifndef CONFIG_H
#define CONFIG_H

// --- WiFi credentials (fill in your own) ---
#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"

// --- Home location (configure with tools/configure.py) ---
#define HOME_LAT 51.0447
#define HOME_LON -114.0719
#define RADIUS_KM 15.0

// --- API endpoints ---
#define AIRPLANE_API_BASE "https://api.airplanes.live/v2/point/51.0447/-114.0719/"
#define PLOW_API_URL     "https://511.alberta.ca/map/mapIcons/ServiceVehicles"

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
#define WX_WIDE_POLL_MS   720000UL  // 12 min; the wide picture changes slowly
#define WX_HISTORY        10        // frames kept for the replay loop
#define WX_REPLAY_STEP_MS 320       // per frame during a replay

// Wide-area sweep: the scope only sees 30 km, which is about 35 minutes of
// warning. This second, coarser fetch answers "is anything heading here?"
#define WX_WIDE_N         32
#define WX_WIDE_BOX_KM    150.0f
#define WX_WIDE_MIN_KM    18.0f     // ignore echoes already inside the scope
#define WX_WIDE_THRESH    46        // 0..255 intensity worth reporting

// --- Night peek ---
#define NIGHT_PEEK_MS    15000

#endif // CONFIG_H
