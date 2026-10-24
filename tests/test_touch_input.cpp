// Compile this with src/touch_input.cpp and -Itests/touch_fakes. Only the
// hardware transport and diagnostics hooks are faked; the driver is real.
#include "touch_fakes/Arduino.h"
#include "touch_fakes/Wire.h"
#include "../src/touch_input.h"
#include "../src/diagnostics.h"
#include <stdio.h>

uint32_t fake_millis = 0;
FakeSerial Serial;
FakeWire Wire;
DiagnosticsPhase::DiagnosticsPhase(const char *) { previous_[0] = '\0'; }
DiagnosticsPhase::~DiagnosticsPhase() {}

static void product() { Wire.responses.push_back({'9', '1', '1', 0}); }
static void status(int value) { Wire.responses.push_back({value}); }
static void coordinates(int x, int y) {
    Wire.responses.push_back({x & 255, x >> 8, y & 255, y >> 8});
}
static void assert_backoff(uint32_t time) {
    fake_millis = time;
    const unsigned before = Wire.transmissions;
    TouchPoint point;
    assert(!touch_input_read(point));
    assert(Wire.transmissions == before);
}

int main() {
    product();
    touch_input_init(19, 45, 480, 480);
    assert(Wire.timeout == 20);
    assert(touch_input_stats().available && touch_input_stats().address == 0x5d);
    TouchPoint point;

    status(0x81); coordinates(20, 30);
    assert(touch_input_read(point) && point.x == 20 && point.y == 30);
    fake_millis = 20; status(0);
    assert(touch_input_read(point)); // held until a new frame or safety timeout
    fake_millis = 120; status(0);
    assert(!touch_input_read(point));

    fake_millis = 121; status(0x85); coordinates(479, 479);
    assert(touch_input_read(point) && point.x == 479 && point.y == 479);
    fake_millis = 130; status(0x80);
    assert(!touch_input_read(point)); // explicit release

    fake_millis = 140; status(0xff);
    const unsigned reads_before_bad_count = Wire.requests;
    assert(!touch_input_read(point));
    assert(Wire.requests == reads_before_bad_count + 1); // no contact array read
    assert(touch_input_stats().invalid_frames == 1);
    assert_backoff(200);
    fake_millis = 390; product(); status(0x81); coordinates(1, 2);
    assert(touch_input_read(point));
    assert(touch_input_stats().recoveries == 1);

    fake_millis = 400; Wire.transmit_results.push_back(4); // NACK before request
    const unsigned reads_before_nack = Wire.requests;
    assert(!touch_input_read(point) && Wire.requests == reads_before_nack);
    assert_backoff(600);
    fake_millis = 650; product(); Wire.responses.push_back({}); // short status
    assert(!touch_input_read(point));
    assert_backoff(900); // second failure must back off 500ms despite good ID

    fake_millis = 1150; product(); status(0x81);
    Wire.responses.push_back({1, 0, 2}); // short coordinate payload
    assert(!touch_input_read(point));
    assert_backoff(2000);
    fake_millis = 2150; product(); status(-1); // declared byte missing on read()
    assert(!touch_input_read(point));
    assert_backoff(4000);

    fake_millis = 4150; product(); status(0x81); coordinates(10, 20);
    Wire.transmit_results = {0, 0, 0, 0, 4}; // failed frame acknowledgement
    assert(!touch_input_read(point));
    assert_backoff(8000);
    fake_millis = 8150; product(); status(0);
    assert(!touch_input_read(point));
    assert(touch_input_stats().available && touch_input_stats().recoveries == 2);

    fake_millis = 8160; status(0x81); coordinates(480, 0);
    assert(!touch_input_read(point));
    assert(touch_input_stats().invalid_frames == 2);

    fake_millis = 8410;
    Wire.transmit_results = {4, 0, 0, 0}; // primary absent, alternate address works
    product(); status(0);
    assert(!touch_input_read(point));
    assert(touch_input_stats().available && touch_input_stats().address == 0x14);

    fake_millis = UINT32_MAX - 100; status(-1);
    assert(!touch_input_read(point));
    assert_backoff(148); // unsigned elapsed=249ms across millis() wrap
    fake_millis = 149; product(); status(0);
    assert(!touch_input_read(point) && touch_input_stats().available);
    assert(Wire.responses.empty() && Wire.transmit_results.empty());
    puts("Touch driver passed transport failures, bounds, hold/release, retry backoff and recovery");
}
