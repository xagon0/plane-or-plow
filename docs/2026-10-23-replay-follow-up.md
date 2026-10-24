# Replay pause and zero-count follow-up — October 23, 2026

After confirming the initial repair's animation and touch worked, the user
reported zero aircraft/plows and a seconds-long pause on a replay tap.

## Independent live counts

At approximately 19:13 MDT, independent HTTP/1.1 queries confirmed that zero
was correct for the configured 15 km radius:

- The aircraft scope query returned HTTP 200 and an empty `ac` array. A wider
  100 nautical mile query returned 27 entries, of which 8 remained after
  excluding surface and on-ground traffic. None were within 15 km. The nearest
  eligible aircraft was 34.47 km away.
- The plow feed returned HTTP 200 with 90 statewide entries and none within
  15 km. The nearest reported plow was 32.37 km away.
- These are snapshots, not a claim that there can never be nearby traffic.
  Sanitized evidence is saved in `diagnostics/counts-check-*.json` beside the
  source checkout. The aircraft provider rejects HTTP/1.0 with 426; both the
  device and successful independent checks use HTTP/1.1.

## Cause of the tap pause

`backlight_on_tap()` started radar playback and immediately marked the contact
poll timer ready. The timer performed a synchronous TLS/HTTP request inside
LVGL's timer handler. The first repair bounded those waits but still paused
clock, touch, pulses and replay until the request finished. Radar's own
periodic requests could also cause short pauses. This was distinct from the
original indefinite body-read hang.

## Repair

Aircraft, plow and radar timers now queue downloads to one shared worker on
core 0. Each request retains the existing transport deadlines and PSRAM size
caps. The worker is separately subscribed to the existing 60-second task
watchdog, feeds while idle, and does not feed during a request.

Only the main task parses JSON/PNG results, merges contacts, updates counters,
and calls LVGL. Response ownership crosses through FreeRTOS queues; the shared
slot stays busy until the main completion callback returns and the response
buffer is freed. This avoids both cross-core renderer races and overlapping
TLS/parser memory allocations. Timers retry a busy slot after 250 ms rather
than colliding at the same 20-second phase forever.

Radar decoded during playback is kept separate and applied after replay ends.
A tap displays the oldest stored frame immediately. The title shows the replay
frame number so empty radar frames still have visible playback feedback. The
layer represents precipitation, not clouds. Compositing skips interpolation
when the whole radar grid is below its visible threshold.

`/status.feed_http` exposes worker state and `/status.render` exposes frame
count and maximum gap since startup. These provide direct evidence of continued
rendering during background requests, alongside a physical touch check.

## Host verification

The integrated ESP32-S3 production build passed. The application regression
suite passed for HTTP bounds/contact expiry, long-job scheduling fairness,
threaded worker ownership/failure cleanup, and actual radar completion during
replay. The worker test deliberately blocks HTTP GET while the main task keeps
ticking, with no watchdog feeds during the request. Tests and sanitizer scope
are documented in [the test guide](../tests/README.md).

The normal production image is 1,353,072 bytes, SHA-256:

```text
2f4fb87e6cd7da876ac18dd350a12862e23b04b768744b1daa1500e20d80118e
```

The build includes no deliberate-stall self-test. The earlier repaired binary
and original firmware remain available locally for rollback.

## Production hardware verification

The image was flashed at `0x10000` over USB at 115200 baud; esptool verified
the written hash. Boot ID `cb855767` remained unchanged through
320 seconds of uptime. Both task watchdogs were armed. After the
status server started, all 150 samples over five minutes succeeded.
There were two expected failed probes before the startup server was ready.

The run recorded 10 successful aircraft polls,
3 successful plow polls and 11 successful
radar fetches, with zero feed failures, zero touch errors and all ten history
frames loaded. The display rendered 2228 frames after startup;
the largest measured frame gap was 195 ms. At
19:24:33–19:24:35, one plow request advanced from 603 ms to 2,642 ms in flight
while the display advanced from frame 645 to frame 658. This directly verifies
that a multi-second download no longer stops rendering. Active requests from
all three feed types were observed while frames continued advancing.

The most recent idle internal heap was 220,032 bytes;
the run's minimum was 154,480 bytes. Temporary TLS allocations
reduce free heap while a download is active, so busy samples are not comparable
to the idle baseline.

The earlier physical touch test passed on the first repaired build. The user
has been asked to recheck the new replay indicator; that follow-up confirmation
has not yet arrived. Host tests exercise replay with synthetic precipitation
and in-flight results. This short run does not establish multi-day reliability.
Evidence is saved in `diagnostics/serial-async.log`, `status-async.ndjson`,
`async-verification.json` and `deployment-async-manifest.json` beside the source.
