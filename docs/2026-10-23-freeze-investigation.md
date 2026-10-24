# October 23, 2026 freeze investigation

The reported display freeze began at 17:58 and the device remained powered
until at least 18:15. Plow positions had appeared more than eight hours old
earlier that day, while the clock was still updating. Times in this report
are local to Alberta, UTC−6 on the investigation date.

The code contained a path that can block the display indefinitely, a separate
defect that can preserve stale contacts indefinitely, and an unchecked touch
read that can corrupt the touch driver's state. These are addressed by this
change. There is no recovered stack trace or core dump
identifying the exact operation at 17:58, so the historical trigger is not
proven.

## Original evidence

- Source was recovered from the iMac checkout of `xagon0/plane-or-plow`,
  clean `main` at `ad7a8bbc820ed8f8b5f0a186c8c7d0402c22a6a1`. Local
  configuration and basemap files were preserved without publishing them.
- The original firmware, ELF, map, bootloader and partition image were
  recovered. The binary was built September 3 at 21:15, after the final
  source edits and before that commit. A serial read of the installed app
  exactly matched this recovered binary. Its SHA-256 is
  `72b399b93302673ed1f5ad07c9b7448a3c27f9d3ee296336177f353db2a92a2c`.
- The installed 64 KiB core-dump partition was read before flashing and was
  entirely erased (`0xff`). The boot region, NVS, partition table, and installed
  app through address `0x160000` were backed up at 115200 baud. Faster serial
  transfers were unreliable; the successful backup SHA-256 is
  `f9afda417419cddf0b2cc4fe47923fe36d3c75606ca6bb16cba2ade27dd65e21`.
- No saved serial log or core dump was found in the original project. The
  later HTTP status snapshot was from a fresh power-on, not the frozen run.
  At 18:20:19 it reported 132 seconds of uptime, `power-on` reset,
  242,492 bytes of free internal heap, a 75,592-byte minimum, and 6,384,767
  bytes of free PSRAM. Six radar fetches, four aircraft polls and one plow
  poll had succeeded. This establishes that the restarted device was working;
  it does not explain the earlier freeze.
- A live 511 request returned HTTP 200 and 258,105 bytes for 104 vehicles.
  Serialized icon metadata accounted for approximately 250 KB. The renderer
  needs only each vehicle's ID and location.

## Freeze mechanism and memory pressure

Aircraft and plow requests ran synchronously inside LVGL timer callbacks.
The pinned Arduino 2.0.17 implementation of `HTTPClient::getString()` can
remain in its body-copy loop indefinitely if a peer sends an incomplete body
and keeps the connection open. The no-data branch does not apply the configured
socket timeout. Such a loop yields to FreeRTOS, so healthy idle tasks do not
necessarily detect that the application loop, clock and display have stopped.
The original firmware did not subscribe the Arduino loop task to its own
progress watchdog.

That is a concrete indefinite-wait defect and a plausible explanation for the
observed frozen clock. No record from 17:58 distinguishes it from every other
possible fault. A network problem, weather change, or upstream response change
over the preceding days has not been established.

The large 511 document was an additional avoidable memory load. A host test
using ArduinoJson 7.4.2 measured a 179,414-byte peak for the full parsed
document, versus 14,686 bytes when retaining only `item2[].itemId` and
`item2[].location`. The full document retained 175,069 bytes, mostly in
allocations no larger than 4,096 bytes. The original SDK prefers internal RAM
for allocations up to that threshold. Host pointer/slot sizes differ from the
ESP32, so these are comparative measurements, not exact device heap figures.
The 258 KB response String was additional storage, normally served from
PSRAM. The payload size alone does not prove internal-heap exhaustion.

## Touch defect exposed during watchdog testing

The first hardware watchdog self-test deliberately stopped loop progress.
The watchdog reset the board after approximately 60 seconds, and the next
boot retained the phase `watchdog-self-test` in RTC memory. This verified
watchdog recovery and breadcrumb retention. The reboot then exposed a flood
of touch I²C errors, prompting inspection of the original touch library.

The recovered iMac and local TAMC_GT911 1.0.2 source files matched
byte-for-byte; the original firmware map confirms that this library was
linked into the installed app. Its `readByteData()` ignores transaction/read
failures and stores `Wire.read()` in a `uint8_t`. A missing byte returns `-1`,
which becomes `0xff`. `read()` interprets the low nibble as 15 touches, then
writes `points[i]` without checking the capacity of `points[5]`.

On this build the overwrite stays within the GT911 object and corrupts
neighboring members, including the I²C address. A transient read failure can
therefore poison subsequent transactions and cause a persistent error storm.
This is a confirmed defect in the original firmware and was exposed during
recovery testing; no retained evidence proves it caused the 17:58 freeze.

The replacement driver in `src/touch_input.cpp` checks each transaction,
requested byte count and read result. It validates the controller identity,
touch count and coordinates, and reads only the first contact needed by LVGL.
It retains the controller's factory configuration, avoiding unavailable reset
pins and configuration writes; it still acknowledges the frame-ready flag.
Reads have a 20 ms bus timeout. Failures release input immediately and retry
with a 250 ms to 5 second backoff. If no new frame arrives for 100 ms, a held
press is released. The TAMC library dependency has been removed.

## Stale plow observations

The old contact-age counter advanced only after successful parsing. HTTP or
JSON failures returned without aging or removing previous contacts. A plow
could therefore remain on screen for hours while the clock continued running.
This defect is independent of the eventual complete display freeze.

There is also an upstream freshness limit. The map response contains
`item2` entries with `itemId`, `location`, `icon.json` and `title`; there is no
per-position observation time. Repeatedly downloading an old position resets
the local receipt time. Unchanged coordinates alone cannot establish staleness,
because a vehicle can legitimately remain parked.

Three website tooltips were checked at approximately 18:24:

| Sample | Website `Updated` time | Approximate age |
| --- | --- | --- |
| Nearest available vehicle | October 23, 17:57 | 27 minutes |
| First entry in saved feed | October 23, 18:22 | 2 minutes |
| Last entry in saved feed | October 23, 18:22 | 2 minutes |

No sampled vehicle was eight hours old. There were no vehicles inside the
configured display radius in the saved response. The sample demonstrates
that some upstream observations can lag, but does not prove the cause of the
reported eight-hour-old display. The timestamp came from the separate HTML
endpoint `/tooltip/ServiceVehicles/{itemId}?lang=en`, table row `Updated`.
[Alberta 511 documents](https://511.alberta.ca/about/snowplows) a normal
20–30-second vehicle update cadence, with possible cellular/GPS delays.

## Changes

- Replace unbounded body copies with a reader that rejects a stalled,
  truncated, oversized or unsupported encoded response. The idle limit is
  2.5 seconds; total body-read limits are 15 seconds for contacts and 9 seconds
  for radar. Maximum body sizes are 256 KiB for aircraft, 1 MiB for plows and
  128 KiB for radar. Separate TLS, connection/header and redirect limits also
  apply. Aircraft retains HTTP/1.1 because the provider rejects HTTP/1.0
  with status 426. Chunked responses use the same total and idle limits,
  with an additional 16 KiB framing limit. Normal TLS closure terminates
  a close-delimited response; JSON/PNG completeness is checked afterward.
  The operations still run synchronously.
- Allocate response bodies explicitly in PSRAM and filter JSON before
  retaining the parsed document. Decode radar from a complete bounded PNG,
  checking dimensions and completion, rather than discarding any suffix that
  the streaming decoder had not yet consumed.
- Expire aircraft contacts after 3 minutes and plows after 10 minutes without
  refresh, even when all requests fail. Expose successful-download ages and
  stale flags in `/status`. These measure transport freshness; source position
  age remains unavailable and is explicitly reported as such. This change
  does not add statewide per-vehicle tooltip requests.
- Subscribe the loop task to a 60-second watchdog and feed it only at progress
  checkpoints. Record phase and progress breadcrumbs in RTC RAM; print
  periodic health information to serial. `/status` includes build identity,
  boot identity, watchdog state and the previous run's breadcrumb.
- Keep breadcrumb writes out of flash. RTC records are unavailable after
  power-off and discarded after power-on/brownout. Serial output is not a
  historical on-device log; an external capture is required to retain it.
- Replace the unchecked TAMC touch driver with validated GT911 reads and
  bounded retries. `/status.touch` reports `available`, `address`, `frames`,
  `errors`, `invalid_frames` and `recoveries` so transport failure and later
  recovery are visible without treating an I²C error as a touch.

## Reproducing host regressions

From the repository root, with a C++17 compiler installed:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -pedantic tests/test_http_body.cpp -o /tmp/planeplow-http-test
/tmp/planeplow-http-test
```

Expected output:

```text
PASS: 30 bounded HTTP body and 2 contact expiry scenarios
```

The tests exercise the actual shared reader with fake sockets, including an
incomplete body whose peer remains connected, a slow trickle that must hit
the total deadline, early close, known/unknown size caps, zero/failed reads,
storage rejection, TLS close-delimited completion, chunked sizes/extensions/
trailers, invalid or missing chunk terminators, bounded chunk metadata, and
`millis()` wraparound. Contact tests cover expiry
boundaries, stable compaction and timer wraparound. These verify the repair's
behavior; they do not replay or prove the historical 17:58 event.

The touch parser test has passed 268 status, length and bounds cases plus
explicit null, release and dimension checks. It covers every possible status
byte, including `0xff`, and preserves the board's coordinate orientation.
Reproduce it with:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -pedantic tests/test_touch_frame.cpp -o /tmp/planeplow-touch-frame-test
/tmp/planeplow-touch-frame-test
```

The transport mock test compiles the actual replacement driver with fake
Arduino/Wire transport and diagnostics hooks:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -pedantic -Itests/touch_fakes tests/test_touch_input.cpp src/touch_input.cpp -o /tmp/planeplow-touch-input-test
/tmp/planeplow-touch-input-test
```

The transport mock and parser passed with AddressSanitizer and
UndefinedBehaviorSanitizer. Cases include NACK, short status/coordinate
reads, `Wire.read() == -1`, acknowledgement failure, invalid touch count
and coordinates, held/released input, retry backoff, alternate address
discovery, recovery, and timer wraparound. The [host test guide](../tests/README.md)
contains the exact sanitizer commands and the buffered PNG regression check.

## Hardware recovery and initial deployment

This initial repair was subsequently extended to remove short replay/network
pauses. See the [replay follow-up](2026-10-23-replay-follow-up.md) for the newer
firmware and its verification; the evidence below records the earlier build.

The hardware watchdog test was repeated with the checked GT911 driver. The
loop deliberately stopped feeding while FreeRTOS idle tasks continued running.
At 18:37:37 the task watchdog reset the board; the next boot reported reset
reason `task-watchdog`, retained `watchdog-self-test` as the previous phase,
and brought GT911 back online at `0x5d` with zero touch errors. Its HTTP status
endpoint responded after reboot. Both test core dumps were saved locally,
and the test core-dump partition was erased before deploying production.

The initial production repair was written at `0x10000` over USB at 115200 baud;
esptool reported **Hash of data verified**. Bootloader, partition table and
NVS configuration were preserved. The deliberately stalled test is not compiled
into this image. The production binary is 1,348,976 bytes, SHA-256:

```text
43c1d46857c1ab6456bb47de93f3eef7c405b8610686bb60c2e40779881af0d9
```

The local `diagnostics/` directory beside the source checkout holds the original
flash backup, production binary/ELF/map, deployment manifest, serial captures,
status samples and the watchdog evidence. The original binary is also retained
in `original-build/` for rollback. These diagnostic files include device/network
configuration and are not committed or uploaded.

Production status was sampled 16 times over five minutes, through 18:52:10
and 330 seconds of uptime. All samples retained boot ID `c45cdcaa`, with the
watchdog armed and no intervening restart. There were 12 successful aircraft
polls, 3 successful plow polls and 11 successful radar fetches, with zero
failures. GT911 remained available with zero transport or invalid-frame errors.
No touch frames were generated during this unattended observation. After
deployment, the user confirmed that display animation and physical touch,
including the brightness strip, both work normally. The clock advanced
normally in status.

Free internal heap settled at 239,588 bytes, with a minimum of 186,412 bytes
and 6,384,767 bytes of free PSRAM. Free heap was essentially flat after radar
history filled. The observed minimum is substantially above the original
75,592-byte snapshot, but the workloads and durations differ; this is not a
controlled multi-day memory comparison.

A short bench run cannot establish reliability over several days. The status
endpoint and serial capture verify loop progress and controller communication;
the user separately confirmed normal display animation and finger input.
