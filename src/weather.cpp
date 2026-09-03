#include "weather.h"
#include "config.h"
#include "ambient.h"
#include "wx_ramp.h"
#include "status.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <pngle.h>
#include <math.h>
#include <time.h>

uint8_t *weather_grid = NULL;
bool     weather_valid = false;

bool  weather_inbound = false;
float weather_inbound_bearing = 0.0f;
float weather_inbound_km = 0.0f;

// ---------------------------------------------------------------------------
// Frame history for the replay loop
// ---------------------------------------------------------------------------
static uint8_t *frames[WX_HISTORY] = {NULL};
static int frame_count = 0;      // valid frames
static int frame_head  = 0;      // index of newest

static uint8_t *wide_grid = NULL;

// ---------------------------------------------------------------------------
// PNG decode target — pngle hands us pixels one at a time
// ---------------------------------------------------------------------------
static uint8_t *decode_dst = NULL;
static int      decode_n   = 0;
static uint32_t decode_hits = 0;

// Nearest neighbour against the EC ramp. The server antialiases when it
// resamples to our bbox, so tiles carry many more colours than the palette.
static inline uint8_t ramp_intensity(uint8_t r, uint8_t g, uint8_t b) {
    int best = 1 << 30, bi = 0;
    for (int i = 0; i < WX_RAMP_N; i++) {
        int dr = (int)r - wx_ramp[i][0];
        int dg = (int)g - wx_ramp[i][1];
        int db = (int)b - wx_ramp[i][2];
        int d = dr * dr + dg * dg + db * db;
        if (d < best) { best = d; bi = i; }
    }
    return (uint8_t)((bi * 255) / (WX_RAMP_N - 1));
}

static void png_draw_cb(pngle_t *p, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h, const uint8_t rgba[4]) {
    (void)p; (void)w; (void)h;
    if ((int)x >= decode_n || (int)y >= decode_n) return;
    uint8_t v = (rgba[3] < 64) ? 0 : ramp_intensity(rgba[0], rgba[1], rgba[2]);
    decode_dst[y * decode_n + x] = v;
    if (v) decode_hits++;
}

// ---------------------------------------------------------------------------
// Fetch one GetMap tile and decode it straight into `dst`.
// `iso_time` may be NULL, which asks the server for its latest frame.
// ---------------------------------------------------------------------------
static bool wx_debug = true;   // first fetch prints diagnostics

static bool fetch_tile(uint8_t *dst, int n, float box_km, const char *iso_time) {
    if (WiFi.status() != WL_CONNECTED) return false;

    float dlat = box_km / 111.32f;
    float dlon = box_km / (111.32f * cosf((float)HOME_LAT * (float)M_PI / 180.0f));

    // WMS 1.3.0 with EPSG:4326 takes bbox in lat,lon order.
    static char url[512];
    int len = snprintf(url, sizeof(url),
        "%s?service=WMS&version=1.3.0&request=GetMap"
        "&layers=%s&styles=%s&crs=EPSG:4326"
        "&bbox=%.4f,%.4f,%.4f,%.4f&width=%d&height=%d&format=image/png",
        WX_HOST, WX_LAYER, WX_STYLE,
        (float)HOME_LAT - dlat, (float)HOME_LON - dlon,
        (float)HOME_LAT + dlat, (float)HOME_LON + dlon,
        n, n);
    if (iso_time && len > 0 && len < (int)sizeof(url)) {
        snprintf(url + len, sizeof(url) - len, "&time=%s", iso_time);
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setUserAgent(HTTP_USER_AGENT);
    http.setTimeout(9000);
    // HTTP/1.0 turns off chunked transfer encoding. We read the body through
    // getStreamPtr() to feed the PNG decoder incrementally, and that stream is
    // raw — chunk-size headers would land in the decoder as garbage. (The plow
    // fetch needs this for the same reason.)
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("Radar HTTP %d\r\n", code);
        Serial.printf("  url: %s\r\n", url);
        stats.radar_failures++;
        http.end();
        return false;
    }
    if (wx_debug) Serial.printf("Radar URL: %s\r\n", url);

    pngle_t *pngle = pngle_new();
    if (!pngle) { http.end(); return false; }

    memset(dst, 0, (size_t)n * n);
    decode_dst = dst;
    decode_n = n;
    decode_hits = 0;
    pngle_set_draw_callback(pngle, png_draw_cb);

    WiFiClient *stream = http.getStreamPtr();
    // Static, not stack: this runs on the Arduino loop task alongside an
    // mbedTLS handshake, and 1.2 KB of locals here is a meaningful slice.
    static uint8_t buf[512];
    static char head[161];
    head[0] = 0;
    int head_len = 0;
    int total = 0;
    bool ok = true;
    int remaining = http.getSize();

    // Both bounds matter. This loop runs inside an LVGL timer callback, so
    // every millisecond spent here is a millisecond the display is frozen.
    const uint32_t started = millis();
    uint32_t last_data = started;

    while (http.connected() && (remaining > 0 || remaining == -1)) {
        if (millis() - started > WX_READ_MS) {
            Serial.println("Radar: read exceeded budget, abandoning tile");
            ok = false;
            break;
        }
        size_t avail = stream->available();
        if (!avail) {
            if (millis() - last_data > WX_STALL_MS) {
                Serial.println("Radar: stream stalled, abandoning tile");
                ok = false;
                break;
            }
            delay(2);
            continue;
        }
        last_data = millis();
        int got = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
        if (got <= 0) break;
        total += got;
        if (head_len < (int)sizeof(head) - 1) {
            int c = got; if (c > (int)sizeof(head) - 1 - head_len) c = sizeof(head) - 1 - head_len;
            memcpy(head + head_len, buf, c); head_len += c; head[head_len] = 0;
        }
        if (pngle_feed(pngle, buf, got) < 0) {
            Serial.printf("Radar PNG error: %s\r\n", pngle_error(pngle));
            Serial.printf("  body head: %s\r\n", head);
            ok = false;
            break;
        }
        if (remaining > 0) remaining -= got;
    }

    pngle_destroy(pngle);
    http.end();
    decode_dst = NULL;

    wx_debug = false;
    stats.radar_fetches++;
    if (!ok) stats.radar_failures++;
    stats.radar_last_ms = millis() - started;
    stats.radar_last_bytes = total;
    if (ok) {
        Serial.printf("Radar tile %dx%d (%.0fkm): %d bytes, %lu cells, %lu ms, "
                      "heap %u, stack left %u\r\n",
                      n, n, box_km * 2.0f, total, (unsigned long)decode_hits,
                      (unsigned long)(millis() - started),
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Wide sweep -> a single "something is heading here" fact
// ---------------------------------------------------------------------------
static void evaluate_inbound() {
    if (!wide_grid) return;

    float cell_km = (WX_WIDE_BOX_KM * 2.0f) / (float)WX_WIDE_N;
    float best_km = 1e9f, best_bearing = 0.0f;

    for (int j = 0; j < WX_WIDE_N; j++) {
        for (int i = 0; i < WX_WIDE_N; i++) {
            if (wide_grid[j * WX_WIDE_N + i] < WX_WIDE_THRESH) continue;
            // Cell centre relative to home; +x east, +y south on the tile.
            float ex = ((float)i + 0.5f - WX_WIDE_N * 0.5f) * cell_km;
            float ny = (WX_WIDE_N * 0.5f - ((float)j + 0.5f)) * cell_km;
            float d = sqrtf(ex * ex + ny * ny);
            if (d < WX_WIDE_MIN_KM || d >= best_km) continue;
            best_km = d;
            float b = atan2f(ex, ny) * 180.0f / (float)M_PI;
            best_bearing = (b < 0.0f) ? b + 360.0f : b;
        }
    }

    weather_inbound = (best_km < WX_WIDE_BOX_KM);
    weather_inbound_km = best_km;
    weather_inbound_bearing = best_bearing;

    if (weather_inbound) {
        Serial.printf("Radar: inbound echo %.0f km, bearing %.0f\r\n",
                      weather_inbound_km, weather_inbound_bearing);
    } else {
        Serial.println("Radar: nothing inbound");
    }
}

// ---------------------------------------------------------------------------
// History
// ---------------------------------------------------------------------------
static void push_frame(const uint8_t *src) {
    frame_head = (frame_count == 0) ? 0 : (frame_head + 1) % WX_HISTORY;
    memcpy(frames[frame_head], src, (size_t)WX_N * WX_N);
    if (frame_count < WX_HISTORY) frame_count++;
}

// Backfill walks further back in time, one frame per tick, so the replay is
// useful within a few minutes of boot instead of an hour.
static int backfill_steps = 0;

// The anchor is captured once. Recomputing from a moving `now` each tick let
// consecutive steps round into the same 6-minute bucket, which fetched the same
// frame twice and left a gap in the history.
static time_t backfill_anchor = 0;

static bool utc_iso_step(int steps_back, char *out, size_t n) {
    if (backfill_anchor == 0) {
        time_t now;
        time(&now);
        if (now < 1700000000) return false;      // NTP has not landed yet
        backfill_anchor = now - 420;             // allow for publishing lag
        backfill_anchor -= backfill_anchor % 360;
    }
    time_t t = backfill_anchor - (time_t)steps_back * 360;
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &g);
    return true;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------
static lv_timer_t *replay_timer = NULL;
static int replay_idx = 0;

static void replay_step_cb(lv_timer_t *t) {
    if (frame_count < 2) { lv_timer_del(t); replay_timer = NULL; return; }

    int oldest = (frame_head - frame_count + 1 + WX_HISTORY) % WX_HISTORY;
    int idx = (oldest + replay_idx) % WX_HISTORY;
    memcpy(weather_grid, frames[idx], (size_t)WX_N * WX_N);
    ambient_scene_dirty();

    if (++replay_idx >= frame_count) {
        // Settle back on the newest frame.
        memcpy(weather_grid, frames[frame_head], (size_t)WX_N * WX_N);
        ambient_scene_dirty();
        lv_timer_del(t);
        replay_timer = NULL;
    }
}

void weather_replay() {
    if (!weather_valid || frame_count < 2 || replay_timer) return;
    replay_idx = 0;
    replay_timer = lv_timer_create(replay_step_cb, WX_REPLAY_STEP_MS, NULL);
    Serial.printf("Radar: replaying %d frames (~%d min)\r\n",
                  frame_count, frame_count * 6);
}

bool weather_replaying() { return replay_timer != NULL; }

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------
static uint32_t last_current_ms = 0;
static uint32_t last_wide_ms = 0;
static uint8_t *scratch = NULL;

// One HTTP request per tick, at most. Fetching happens on the LVGL timer, so
// each round trip stalls the display for its duration; doing three back to back
// would be a visible freeze rather than a hitch.
static void wx_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (WiFi.status() != WL_CONNECTED) return;   // retry on the next tick
    if (weather_replaying()) return;             // don't rewrite the grid mid-replay

    if (!scratch) {
        scratch = (uint8_t *)heap_caps_malloc((size_t)WX_N * WX_N, MALLOC_CAP_SPIRAM);
        if (!scratch) return;
    }

    uint32_t now = millis();

    if (!weather_valid || (now - last_current_ms) >= WX_POLL_MS) {
        last_current_ms = now;
        if (fetch_tile(scratch, WX_N, WX_BOX_KM, NULL)) {
            push_frame(scratch);
            memcpy(weather_grid, scratch, (size_t)WX_N * WX_N);
            weather_valid = true;
            ambient_scene_dirty();
        }
        return;
    }

    // Backfill walks back in time one frame per tick, so a replay is worth
    // watching a few minutes after boot instead of an hour.
    if (frame_count < WX_HISTORY) {
        backfill_steps++;
        char iso[32];
        if (utc_iso_step(backfill_steps, iso, sizeof(iso))) {
            if (fetch_tile(scratch, WX_N, WX_BOX_KM, iso)) {
                int oldest = (frame_head - frame_count + 1 + WX_HISTORY) % WX_HISTORY;
                int slot = (oldest - 1 + WX_HISTORY) % WX_HISTORY;
                memcpy(frames[slot], scratch, (size_t)WX_N * WX_N);
                frame_count++;
                Serial.printf("Radar: backfilled %s (%d frames held)\r\n", iso, frame_count);
            }
        } else {
            backfill_steps--;        // NTP not up yet; try the same step again
        }
        return;
    }

    // last_wide_ms == 0 means we have never swept; do it as soon as the
    // backfill is out of the way rather than waiting out a full interval.
    if (wide_grid && (last_wide_ms == 0 || (now - last_wide_ms) >= WX_WIDE_POLL_MS)) {
        last_wide_ms = now;
        if (fetch_tile(wide_grid, WX_WIDE_N, WX_WIDE_BOX_KM, NULL)) evaluate_inbound();
    }
}

// ---------------------------------------------------------------------------
void weather_init() {
#if !WX_ENABLED
    return;
#endif
    size_t n = (size_t)WX_N * WX_N;
    weather_grid = (uint8_t *)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    wide_grid = (uint8_t *)heap_caps_calloc(1, (size_t)WX_WIDE_N * WX_WIDE_N,
                                            MALLOC_CAP_SPIRAM);
    for (int i = 0; i < WX_HISTORY; i++) {
        frames[i] = (uint8_t *)heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
        if (!frames[i]) { Serial.println("Radar: frame alloc failed"); return; }
    }
    if (!weather_grid || !wide_grid) {
        Serial.println("Radar: grid alloc failed");
        return;
    }

    // weather_init() runs before network_init(), so the first ticks fire with
    // WiFi still down. A short tick period lets it retry, backfill quickly, and
    // then settle into fetching only on the 6-minute scan cadence.
    lv_timer_create(wx_tick_cb, WX_TICK_MS, NULL);

    Serial.printf("Radar: %dx%d over %.0f km, %d-frame history\r\n",
                  WX_N, WX_N, WX_BOX_KM * 2.0f, WX_HISTORY);
}
