// Compile the actual weather implementation with fake hardware/timers and
// the real PNG decoder. Fixed public test configuration avoids secrets.h.
#define CONFIG_H
#define HOME_LAT 0.0
#define HOME_LON 0.0
#define WX_HOST "https://example.invalid/geomet"
#define WX_LAYER "test-radar"
#define WX_STYLE "test-style"
#define WX_ENABLED 1
#define WX_N 64
#define WX_BOX_KM 16.5f
#define WX_HISTORY 10
#define WX_REPLAY_STEP_MS 320
#define WX_POLL_MS 360000UL
#define WX_TICK_MS 20000
#define WX_READ_MS 9000
#define WX_WIDE_N 32
#define WX_WIDE_BOX_KM 150.0f
#define WX_WIDE_MIN_KM 18.0f
#define WX_WIDE_THRESH 46
#define WX_WIDE_POLL_MS 720000UL

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>
#include "../src/weather.cpp"

uint32_t fake_weather_millis = 0;
WeatherFakeSerial Serial;
WeatherFakeESP ESP;
WeatherFakeWiFi WiFi;
SysStats stats;
static std::vector<std::unique_ptr<lv_timer_t>> timers;
static FeedHttpRequest submitted;
static FeedHttpCallback completion = nullptr;
static bool transport_busy = false;
static unsigned submissions = 0, dirty_count = 0;

DiagnosticsPhase::DiagnosticsPhase(const char *) { previous_[0] = 0; }
DiagnosticsPhase::~DiagnosticsPhase() {}
void ambient_scene_dirty() { ++dirty_count; }

lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), uint32_t period, void *) {
    timers.emplace_back(new lv_timer_t{callback, period, true, 0, 0});
    return timers.back().get();
}
void lv_timer_del(lv_timer_t *timer) { timer->active = false; }
void lv_timer_set_period(lv_timer_t *timer, uint32_t period) { timer->period = period; }
void lv_timer_reset(lv_timer_t *timer) {
    timer->last_reset_ms = fake_weather_millis;
    ++timer->resets;
}
bool feed_http_busy() { return transport_busy; }
bool feed_http_submit(const FeedHttpRequest &request, FeedHttpCallback callback) {
    if (transport_busy) return false;
    transport_busy = true;
    submitted = request;
    completion = callback;
    ++submissions;
    return true;
}

static void deliver(const std::vector<char> &png, int code = 200, bool body_ok = true) {
    assert(transport_busy && completion);
    // Match the real worker: callback is on the UI thread, slot stays busy,
    // and the response buffer is destroyed immediately after callback return.
    std::vector<char> worker_buffer = png;
    FeedHttpResult response;
    response.code = code;
    response.body_ok = body_ok;
    response.data = worker_buffer.data();
    response.size = worker_buffer.size();
    response.elapsed_ms = 321;
    const auto callback = completion;
    completion = nullptr;
    callback(response);
    assert(transport_busy);
    std::fill(worker_buffer.begin(), worker_buffer.end(), char(0xa5));
    transport_busy = false;
}

static lv_timer_t *reset(unsigned history = 0) {
    free(weather_grid); weather_grid = nullptr;
    free(wide_grid); wide_grid = nullptr;
    free(scratch); scratch = nullptr;
    for (auto &frame : frames) { free(frame); frame = nullptr; }
    frame_count = frame_head = backfill_steps = replay_idx = 0;
    replay_timer = nullptr;
    fetch_timer = nullptr;
    pending_job = completed_job = RadarJob::None;
    pending_iso[0] = 0;
    last_current_ms = last_wide_ms = 0;
    weather_valid = weather_inbound = false;
    weather_inbound_km = weather_inbound_bearing = 0;
    fake_weather_millis = WX_POLL_MS + 1000;
    backfill_anchor = 1700000640;
    stats = {};
    timers.clear();
    transport_busy = false;
    completion = nullptr;
    submissions = dirty_count = 0;
    WiFi.connection = WL_CONNECTED;
    weather_init();
    assert(timers.size() == 1);
    assert(fetch_timer == timers[0].get());
    for (unsigned i = 0; i < history; ++i) {
        std::fill(weather_grid, weather_grid + WX_N * WX_N, uint8_t((i + 1) * 10));
        push_frame(weather_grid);
    }
    weather_valid = history != 0;
    return timers[0].get();
}

static std::vector<uint8_t> visible() {
    return {weather_grid, weather_grid + WX_N * WX_N};
}
static void finish_replay() {
    unsigned steps = 0;
    while (weather_replaying()) {
        assert(++steps <= WX_HISTORY);
        fake_weather_millis += WX_REPLAY_STEP_MS;
        replay_step_cb(replay_timer);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    std::ifstream stream(argv[1], std::ios::binary);
    const std::vector<char> png((std::istreambuf_iterator<char>(stream)), {});
    assert(png.size() > 24);

    // A current tile arriving mid-replay must not change the displayed old
    // frame, history length, replay index or decode storage before replay ends.
    lv_timer_t *poll = reset(3);
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::Current && transport_busy);
    weather_replay();
    assert(weather_replay_frame() == 1 && weather_grid[0] == 10);
    const auto first_frame = visible();
    deliver(png);
    assert(pending_job == RadarJob::None && completed_job == RadarJob::Current);
    assert(visible() == first_frame && frame_count == 3 && weather_replay_frame() == 1);
    const std::vector<uint8_t> decoded(scratch, scratch + WX_N * WX_N);
    assert(decoded != first_frame);
    const unsigned requests = submissions;
    wx_tick_cb(poll);
    assert(submissions == requests);
    finish_replay();
    assert(weather_grid[0] == 30 && frame_count == 3);
    weather_tick();
    assert(visible() == decoded && frame_count == 4 && completed_job == RadarJob::None);
    weather_tick();
    assert(frame_count == 4); // completion applies exactly once

    // Backfill also waits for replay. Its decoded frame becomes the oldest,
    // preserving the newest visible frame and the saved timestamp's identity.
    poll = reset(3);
    last_current_ms = fake_weather_millis;
    last_wide_ms = fake_weather_millis;
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::Backfill && pending_iso[0] && backfill_steps == 1);
    const std::string queued_time = pending_iso;
    weather_replay();
    deliver(png);
    const std::vector<uint8_t> old_tile(scratch, scratch + WX_N * WX_N);
    assert(completed_job == RadarJob::Backfill && frame_count == 3 && weather_grid[0] == 10);
    // The shared worker may service a different source while radar waits.
    FeedHttpRequest other;
    other.label = "aircraft";
    other.max_bytes = 1024;
    strlcpy(other.url, "https://example.invalid/aircraft", sizeof(other.url));
    assert(feed_http_submit(other, [](const FeedHttpResult &) {}));
    deliver(std::vector<char>{'x'});
    assert(std::equal(old_tile.begin(), old_tile.end(), scratch));
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::None && queued_time == pending_iso);
    finish_replay();
    weather_tick();
    assert(frame_count == 4 && weather_grid[0] == 30);
    const int oldest = (frame_head - frame_count + 1 + WX_HISTORY) % WX_HISTORY;
    assert(std::equal(old_tile.begin(), old_tile.end(), frames[oldest]));
    weather_replay();
    assert(weather_replay_frame() == 1 && visible() == old_tile);
    finish_replay();

    // A busy contact request only defers radar briefly; it consumes no radar
    // attempt or history step. The next free retry submits the correct job.
    poll = reset();
    transport_busy = true;
    wx_tick_cb(poll);
    assert(poll->period == 250 && submissions == 0 && pending_job == RadarJob::None);
    transport_busy = false;
    wx_tick_cb(poll);
    assert(poll->period == WX_TICK_MS && submissions == 1 && pending_job == RadarJob::Current);
    deliver(png);
    assert(weather_valid && frame_count == 1 && stats.radar_fetches == 1);

    // Unavailable historical tiles must not suppress a due wide/inbound scan.
    // With incomplete history, wide gets its turn before another backfill.
    poll = reset(3);
    last_current_ms = fake_weather_millis;
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::Wide && backfill_steps == 0);
    const uint32_t wide_submitted = last_wide_ms;
    assert(wide_submitted == fake_weather_millis);
    fake_weather_millis += 25000; // longer than the regular polling interval
    deliver({}, 503, false);
    assert(poll->period == WX_TICK_MS && poll->resets == 1);
    assert(poll->last_reset_ms == fake_weather_millis);
    assert(poll->last_reset_ms - wide_submitted == 25000);
    // The complete interval now starts after completion, giving a contact
    // request waiting on the shared slot a chance before radar is due again.
    assert(poll->last_reset_ms + poll->period == fake_weather_millis + WX_TICK_MS);
    fake_weather_millis += WX_TICK_MS;
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::Backfill && backfill_steps == 1);
    deliver({}, 503, false);
    assert(frame_count == 3);
    fake_weather_millis += WX_WIDE_POLL_MS;
    last_current_ms = fake_weather_millis; // current feed remains healthy
    wx_tick_cb(poll);
    assert(pending_job == RadarJob::Wide && backfill_steps == 1);
    deliver({}, 503, false);

    // Errors complete the pending job but never replace a valid visible frame.
    poll = reset(3);
    wx_tick_cb(poll);
    const auto before_error = visible();
    deliver({}, 503, false);
    assert(pending_job == RadarJob::None && completed_job == RadarJob::None);
    assert(visible() == before_error && stats.radar_failures == 1 && stats.radar_fetches == 0);
    fake_weather_millis += WX_POLL_MS;
    wx_tick_cb(poll);
    std::vector<char> truncated = png;
    truncated.resize(truncated.size() - 12);
    deliver(truncated);
    assert(visible() == before_error && frame_count == 3);
    assert(stats.radar_failures == 2 && stats.radar_fetches == 1);

    reset();
    std::cout << "PASS: actual weather async replay/lifetime, backfill, wide priority, completion spacing and errors\n";
}
