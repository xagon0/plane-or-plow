# Host regression tests

Run these commands from the repository root. They require a C/C++ compiler
(`cc` and `c++`) and do not connect to or flash the ESP32.

## HTTP body bounds and contact expiry

```sh
c++ -std=c++11 -Wall -Wextra -Werror tests/test_http_body.cpp -o /tmp/planeplow-test-http-body
/tmp/planeplow-test-http-body
```

Expected result:

```text
PASS: 30 bounded HTTP body and 2 contact expiry scenarios
```

The tests exercise the same helpers used by the firmware: completed and
unknown-length responses, stalled open connections, trickled bodies reaching
the total deadline, truncated bodies, declared and undeclared size limits,
zero/failed reads, storage failure, timer wraparound, a 258,105-byte response,
and contact expiry with stable compaction at the age boundary and across timer
wraparound. They also reproduce ESP32 TLS closure reported as a negative
`available()` result: close-delimited responses complete, while missing
declared bytes remain an error.

HTTP/1.1 chunked-response tests cover extensions and trailers, framing split
across individual bytes, successful termination while the socket stays open,
missing terminal chunks, malformed CRLF and hexadecimal sizes, payload and
metadata limits, oversized lines, stalled chunks, and trickled metadata that
must obey the original total deadline. Chunk framing is removed before the
decoded payload size limit and JSON/PNG validation apply.

## Buffered PNG decoding

The PNG test uses the project's installed `pngle` sources. If
`.pio/libdeps/esp32s3/pngle` does not exist yet, install the dependencies with
`pio pkg install -e esp32s3` first.

```sh
cc -c .pio/libdeps/esp32s3/pngle/src/pngle.c -o /tmp/planeplow-pngle.o
cc -c .pio/libdeps/esp32s3/pngle/src/miniz.c -o /tmp/planeplow-miniz.o
c++ -std=c++11 -Wall -Wextra -Werror -I.pio/libdeps/esp32s3/pngle/src tests/test_png.cpp /tmp/planeplow-pngle.o /tmp/planeplow-miniz.o -o /tmp/planeplow-test-png
/tmp/planeplow-test-png tests/fixtures/radar-synthetic.png
```

Expected result:

```text
PASS: buffered PNG decode and truncated IEND rejection
```

The synthetic fixture is a 64×64 RGBA image with varied colors. The test
requires all 4,096 pixels and the decoder's completion callback, then removes
the final PNG chunk and verifies that completion is no longer reported.
Additional 64×64 PNG files can be passed as further arguments to the test
binary. During diagnosis, the current 96-byte GeoMet tile also passed this
check; that location-specific download is not included in the repository.

These host tests cover transfer and decoding behavior. Hardware watchdog
recovery, actual feeds, display updates, and sustained runtime still require
validation on the board.

## GT911 touch packet parsing

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/test_touch_frame.cpp -o /tmp/planeplow-touch-frame-test
/tmp/planeplow-touch-frame-test
```

Expected result:

```text
Touch parser passed 268 status/length/bounds checks plus null/release/dimension checks
```

This exercises the actual parser for every possible status byte, including
the old failed-read value `0xff`, all touch counts, missing or short coordinate
payloads, coordinates outside the display, and the unchanged raw X/Y mapping.
Invalid frames must leave the output point untouched.

## GT911 checked transport and recovery

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/touch_fakes tests/test_touch_input.cpp src/touch_input.cpp -o /tmp/planeplow-touch-input-test
/tmp/planeplow-touch-input-test
```

Expected result:

```text
Touch driver passed transport failures, bounds, hold/release, retry backoff and recovery
```

The production `touch_input.cpp` runs unchanged against a scripted Wire bus,
clock, and serial sink. Diagnostics phase hooks are stubbed. Scenarios cover
NACKs, short status and coordinate reads, `Wire.read()` returning `-1`, failed
frame acknowledgements, invalid contact counts and coordinates, a held contact
without a new frame, explicit release, the 100 ms release timeout, exponential
retry backoff, recovery after errors, the alternate `0x14` address, and retry
timing across `millis()` wraparound. The test verifies that retries perform no
I2C transactions during backoff and that an invalid count never triggers a
coordinate-array read.

Both touch test commands passed on 2026-10-23 with AddressSanitizer and
UndefinedBehaviorSanitizer enabled, with no sanitizer findings. These tests
do not verify physical touch-controller behavior; that is checked on the
board separately.

## Shared feed polling schedule

```sh
c++ -std=c++11 -Wall -Wextra -Werror tests/test_poll_schedule.cpp -o /tmp/planeplow-test-poll-schedule
/tmp/planeplow-test-poll-schedule
```

Expected result:

```text
PASS: shared-slot collision, boost scheduling, and slow-job fairness
```

The production contact scheduling helper is exercised against recurring
transport contention. Rejected submissions must retry after 250 ms without
consuming an aircraft/plow turn, preserve the three-aircraft/one-plow cadence,
and restore the requested regular or boosted interval after completion.
Long-running jobs retain ownership and restart their timer after completion;
simulated requests longer than the regular/boost interval must still let both
contending sources take turns.

## Async radar completion and replay

This test compiles the actual `weather.cpp` and real PNG decoder. Hardware,
LVGL timer control, the clock and shared HTTP transport are stubbed. Its fixed
test configuration uses no private location or WiFi configuration.

```sh
cc -fsanitize=address -c .pio/libdeps/esp32s3/pngle/src/pngle.c -o /tmp/planeplow-weather-pngle.o
cc -fsanitize=address -c .pio/libdeps/esp32s3/pngle/src/miniz.c -o /tmp/planeplow-weather-miniz.o
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Itests/weather_fakes -I.pio/libdeps/esp32s3/pngle/src tests/test_weather_async.cpp /tmp/planeplow-weather-pngle.o /tmp/planeplow-weather-miniz.o -o /tmp/planeplow-test-weather-async
/tmp/planeplow-test-weather-async tests/fixtures/radar-synthetic.png
```

Expected result:

```text
PASS: actual weather async replay/lifetime, backfill, wide priority, completion spacing and errors
```

Current and historical responses arrive while replay is active. The tests
verify that display/history changes wait until replay ends, the decoded result
survives destruction of the worker-owned response buffer, another feed can use
the transport while radar waits, and completion applies exactly once. They
also check the immediate oldest-frame display and replay counter, contention
retry, a due wide scan with incomplete/failed backfill, timer spacing from a
slow request's completion, and HTTP/truncated-PNG failures preserving the
last valid frame.

The application test uses AddressSanitizer and UndefinedBehaviorSanitizer;
the pinned C decoder uses AddressSanitizer. Enabling UndefinedBehaviorSanitizer
on pngle 1.1.0 also reports its existing signed left shift in `read_uint32`
(`pngle.c:144`). That third-party warning is separate from the tested async
state/ownership changes and is not suppressed in application code.

## Background feed worker lifecycle

```sh
c++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread -Itests/feed_fakes tests/test_feed_http.cpp -o /tmp/planeplow-test-feed-http
/tmp/planeplow-test-feed-http
```

Expected result:

```text
PASS: feed worker ownership, main-thread callbacks, blocked-transport responsiveness, watchdog discipline and failure cleanup
```

This compiles the actual `feed_http.cpp` against threaded queue/task and HTTP
transport fakes. It deliberately blocks the worker inside `HTTPClient::GET()`
while the main thread continues ticking, then verifies delivery on the main
thread, no watchdog feeds inside the blocked request, and exactly one active
request. It checks that URL and label storage are copied, clients are destroyed
before the callback, the body remains alive throughout the callback, nested
submissions are rejected until parsing finishes, and each body is freed once.
Failure cases include queue/task allocation, watchdog registration, initial
enqueue, HTTP 429, a partial body, and completion enqueue. The test substitutes
the transport; the separate HTTP body tests cover byte-transfer deadlines and
framing, and physical render timing still requires the board.

The worker lifecycle test passed on 2026-10-23 under AddressSanitizer and
UndefinedBehaviorSanitizer, with no sanitizer findings.
