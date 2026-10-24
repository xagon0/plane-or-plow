#include "weather.h"
#include "config.h"
#include "ambient.h"
#include "wx_ramp.h"
#include "status.h"
#include "feed_http.h"
#include "diagnostics.h"
#include <Arduino.h>
#include <WiFi.h>
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
static bool decode_done = false;

static void png_done_cb(pngle_t *) { decode_done = true; }

static uint32_t png_u32(const char *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

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
// Decode a completed response on the UI task. The HTTP worker owns the
// response buffer until this callback returns; no renderer state crosses cores.
// ---------------------------------------------------------------------------
static bool decode_tile(const FeedHttpResult &response, uint8_t *dst, int n) {
    DiagnosticsPhase phase("radar_decode");
    const uint32_t started = millis();
    stats.radar_last_ms = response.elapsed_ms;
    if (response.code != 200) {
        Serial.printf("Radar HTTP %d\r\n", response.code);
        stats.radar_failures++;
        return false;
    }
    stats.radar_fetches++;
    stats.radar_last_bytes = response.size;
    bool ok = response.body_ok;
    // Validate dimensions before asking pngle to allocate row buffers or
    // decompress. An HTML error page or wrong-sized PNG is not a radar frame.
    static const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!ok || response.size < 24 ||
        memcmp(response.data, signature, sizeof(signature)) != 0 ||
        memcmp(response.data + 12, "IHDR", 4) != 0 ||
        png_u32(response.data + 16) != (uint32_t)n ||
        png_u32(response.data + 20) != (uint32_t)n) {
        Serial.println("Radar: incomplete body or invalid PNG dimensions");
        stats.radar_failures++;
        stats.radar_last_ms = response.elapsed_ms + millis() - started;
        return false;
    }

    pngle_t *pngle = pngle_new();
    if (!pngle) { stats.radar_failures++; return false; }

    memset(dst, 0, (size_t)n * n);
    decode_dst = dst;
    decode_n = n;
    decode_hits = 0;
    decode_done = false;
    pngle_set_draw_callback(pngle, png_draw_cb);
    pngle_set_done_callback(pngle, png_done_cb);
    // Feed the complete bounded PSRAM payload. pngle can leave a partial PNG
    // chunk unconsumed; the old per-socket-read loop discarded that suffix.
    int consumed = pngle_feed(pngle, response.data, response.size);
    ok = consumed >= 0 && (size_t)consumed == response.size && decode_done;
    if (!ok) {
        const char *error = pngle_error(pngle);
        Serial.printf("Radar: invalid/incomplete PNG: %s\r\n",
                      error ? error : "missing end marker");
    }

    pngle_destroy(pngle);
    decode_dst = NULL;

    if (!ok) stats.radar_failures++;
    stats.radar_last_ms = response.elapsed_ms + millis() - started;
    if (ok) {
        Serial.printf("Radar tile %dx%d: %u bytes, %lu cells, %lu ms, "
                      "heap %u, stack left %u\r\n",
                      n, n, (unsigned)response.size, (unsigned long)decode_hits,
                      (unsigned long)stats.radar_last_ms,
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
    replay_step_cb(replay_timer); // show the oldest stored frame immediately
}

bool weather_replaying() { return replay_timer != NULL; }
int weather_replay_frame() { return replay_timer ? (replay_idx ? replay_idx : 1) : 0; }
int weather_history_frames() { return frame_count; }

// ---------------------------------------------------------------------------
// Poll
// ---------------------------------------------------------------------------
static lv_timer_t *fetch_timer = nullptr;
static uint32_t last_current_ms = 0;
static uint32_t last_wide_ms = 0;
static uint8_t *scratch = NULL;

enum class RadarJob { None, Current, Backfill, Wide };
static RadarJob pending_job = RadarJob::None;
static RadarJob completed_job = RadarJob::None;
static char pending_iso[32] = {};

// Apply completed downloads on the UI task, after any replay finishes. The
// shared worker can keep fetching contacts while the decoded radar waits.
void weather_tick() {
    if (completed_job == RadarJob::None || weather_replaying()) return;
    if (completed_job == RadarJob::Current) {
        push_frame(scratch);
        memcpy(weather_grid, scratch, (size_t)WX_N * WX_N);
        weather_valid = true;
        ambient_scene_dirty();
    } else if (completed_job == RadarJob::Backfill) {
        int oldest = (frame_head - frame_count + 1 + WX_HISTORY) % WX_HISTORY;
        int slot = (oldest - 1 + WX_HISTORY) % WX_HISTORY;
        memcpy(frames[slot], scratch, (size_t)WX_N * WX_N);
        frame_count++;
        Serial.printf("Radar: backfilled %s (%d frames held)\r\n", pending_iso, frame_count);
    } else if (completed_job == RadarJob::Wide) {
        evaluate_inbound();
    }
    completed_job = RadarJob::None;
}

static void radar_complete(const FeedHttpResult &response) {
    // Start the next interval at completion, so a slow request cannot reclaim
    // the slot immediately ahead of a waiting contact request.
    if (fetch_timer) {
        lv_timer_set_period(fetch_timer, WX_TICK_MS);
        lv_timer_reset(fetch_timer);
    }
    bool wide = pending_job == RadarJob::Wide;
    if (decode_tile(response, wide ? wide_grid : scratch, wide ? WX_WIDE_N : WX_N))
        completed_job = pending_job;
    pending_job = RadarJob::None;
    weather_tick();
}

static bool queue_tile(RadarJob kind, int n, float box_km, const char *iso) {
    FeedHttpRequest request;
    request.label = "radar";
    request.max_bytes = 128 * 1024;
    request.body_timeout_ms = WX_READ_MS;
    float dlat = box_km / 111.32f;
    float dlon = box_km / (111.32f * cosf((float)HOME_LAT * (float)M_PI / 180.0f));
    int len = snprintf(request.url, sizeof(request.url),
        "%s?service=WMS&version=1.3.0&request=GetMap"
        "&layers=%s&styles=%s&crs=EPSG:4326"
        "&bbox=%.4f,%.4f,%.4f,%.4f&width=%d&height=%d&format=image/png",
        WX_HOST, WX_LAYER, WX_STYLE,
        (float)HOME_LAT - dlat, (float)HOME_LON - dlon,
        (float)HOME_LAT + dlat, (float)HOME_LON + dlon, n, n);
    if (len < 0 || len >= (int)sizeof(request.url)) return false;
    if (iso) {
        int added = snprintf(request.url + len, sizeof(request.url) - len, "&time=%s", iso);
        if (added < 0 || added >= (int)(sizeof(request.url) - len)) return false;
    }
    if (!feed_http_submit(request, radar_complete)) return false;
    pending_job = kind;
    strlcpy(pending_iso, iso ? iso : "", sizeof(pending_iso));
    return true;
}

// Timers only enqueue work; TLS and socket waits never run inside LVGL.
static void wx_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (WiFi.status() != WL_CONNECTED || weather_replaying() ||
        pending_job != RadarJob::None || completed_job != RadarJob::None) {
        lv_timer_set_period(timer, WX_TICK_MS);
        return;
    }
    // Contact and radar timers can become due together. Retry briefly after
    // contention so the earlier timer cannot starve the other source.
    if (feed_http_busy()) { lv_timer_set_period(timer, 250); return; }
    lv_timer_set_period(timer, WX_TICK_MS);
    if (!scratch) {
        scratch = (uint8_t *)heap_caps_malloc((size_t)WX_N * WX_N, MALLOC_CAP_SPIRAM);
        if (!scratch) return;
    }
    uint32_t now = millis();
    if (!weather_valid || (now - last_current_ms) >= WX_POLL_MS) {
        if (queue_tile(RadarJob::Current, WX_N, WX_BOX_KM, nullptr)) last_current_ms = now;
        return;
    }
    // A failed historical tile must not postpone the independent wide scan
    // indefinitely. Service it when due, before trying another history frame.
    if (wide_grid && (last_wide_ms == 0 || (now - last_wide_ms) >= WX_WIDE_POLL_MS)) {
        if (queue_tile(RadarJob::Wide, WX_WIDE_N, WX_WIDE_BOX_KM, nullptr)) last_wide_ms = now;
        return;
    }
    if (frame_count < WX_HISTORY) {
        char iso[32];
        if (utc_iso_step(backfill_steps + 1, iso, sizeof(iso)) &&
            queue_tile(RadarJob::Backfill, WX_N, WX_BOX_KM, iso)) ++backfill_steps;
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
    fetch_timer = lv_timer_create(wx_tick_cb, WX_TICK_MS, NULL);

    Serial.printf("Radar: %dx%d over %.0f km, %d-frame history\r\n",
                  WX_N, WX_N, WX_BOX_KM * 2.0f, WX_HISTORY);
}
