#ifndef CONFIG_H
#define CONFIG_H

// --- WiFi credentials (fill in your own) ---
#define WIFI_SSID "your-network"
#define WIFI_PASS "your-password"

// --- Home location (NW Calgary) ---
#define HOME_LAT 51.0447
#define HOME_LON -114.0719
#define RADIUS_KM 15.0

// --- API endpoints ---
#define AIRPLANE_API_BASE "https://api.airplanes.live/v2/point/51.0447/-114.0719/"
#define PLOW_API_URL     "https://511.alberta.ca/map/mapIcons/ServiceVehicles"

// --- Timing ---
#define API_POLL_MS       45000   // Poll one source every 45s (each source every 90s)
#define BOOST_POLL_MS     15000   // 15s per tick during boost (each source every 30s)
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

// --- Night peek ---
#define NIGHT_PEEK_MS    15000

#endif // CONFIG_H
