# Experimental Bluetooth Photo v2

This is a local development implementation, disabled in normal release builds.
It has host and build validation; BLE timing, power use and physical panel
output still require hardware acceptance. It does not require server pairing.

All four `-ble-photo` envs explicitly disable `TESSERAE_OTA_CAPABILITY_ENABLED`,
so ordinary server OTA cannot replace an experimental image even after
switching its screen mode back to Automatic. CI builds them in a separate
compile-only matrix, without publishing release/OTA artifacts. They reuse the
normal board kind only for pixel/device identity.
Returning to a normal OTA-enabled firmware requires an explicit reflash.
Normal envs retain their existing OTA capability and release behavior.

## User flow and lifetime

1. Hold Refresh for 3–19 seconds and release to open Maintenance. Authenticate
   through the existing QR or encrypted, MITM-authenticated passkey channel.
2. Select Bluetooth mode in Companion. Firmware atomically saves manual mode
   and a 32-byte photo key, then returns `screen_mode`. The phone saves this key.
3. Leave Maintenance. Disconnect, physical exit or its absolute five-minute
   deadline reboots into the saved mode, giving the phone time to store the ACK.
4. In Bluetooth mode, a short button wake opens a photo session. It advertises
   for at most 90 seconds before authenticated commands, five minutes of idle
   time after authentication, and ten minutes absolutely. Opening a session
   does not paint another QR or connect Wi-Fi.
5. Send a photo. Once its bytes and SHA-256 are verified, firmware stops BLE,
   paints with the existing full-frame driver, clears the server ETag and sleeps.
   An immediate phone disconnect after commit does not discard that frame.
6. Hold Refresh again to enter Maintenance and select Automatic/Wi-Fi. Firmware
   revokes the photo key and reboots when Maintenance closes. Normal configured
   transport resumes and fetches a fresh frame.

A device without server registration can authorize Bluetooth mode by holding
Refresh through power-on/RESET for 3–19 seconds and releasing into Maintenance.
The existing captive-portal awake-window gesture still opens new-device setup;
it does not offer the manual-mode picker. No server pairing is needed in the
Maintenance authorization or photo path.

Bluetooth mode bypasses all automatic REST/MQTT/relay/deck updates, always-on
operation and the USB development loop. Normal manual sleep has neither BLE
advertising nor Wi-Fi, and wakes daily for a battery check or on a physical
button. Existing low-battery goodbye/recovery uses its hourly recheck; a photo
begin is refused for a known battery below 3450 mV. Unknown/mains readings retain
existing behavior. Holding Refresh for 20 seconds retains factory reset.
Manual mode has no touch-based photo entry or touch wake. Its sleep path calls
the existing `touch_park_idle()` on touch boards: this initializes the E1003's
permanently powered GT911 if necessary, then commands sleep, so skipping normal
touch initialization cannot leave it scanning for the entire manual sleep.
Buttons and the daily battery timer remain the wake sources.

The mode and authorization survive reboot. A received frame is held in RAM
until painting, not saved as a photo file. The entry instruction is painted
only once after a deliberate Automatic-to-Bluetooth transition. A one-shot NVS
flag is consumed and committed **before** drawing, so a later cold boot,
brownout, watchdog, panic or battery recovery does not redraw that instruction
over a retained photo. If consuming the flag fails, the device neither paints
nor accepts a new photo on that wake; retry after recovery or enter Maintenance.
Power loss between the consume commit and drawing may omit that one instruction;
this at-most-once choice protects retained photos. Explicit Maintenance and
existing critical-battery protection can still replace the physical image.
Maintenance exit without changing mode keeps manual mode.

Mode storage is one versioned NVS blob (`ble_photo/config`): version byte 2,
mode byte 0/1, 32 key bytes, then one byte `prompt_pending` (0/1). Mode, key and
prompt are committed together. Absent configuration means Automatic. The old
34-byte v1 blob remains readable; an existing v1 Manual configuration has no
pending prompt, and its next successful mode write migrates it to v2. Repeated
Bluetooth authorization preserves the active key and pending/consumed prompt.
Selecting Wi-Fi blindly commits mode 0, a zeroed key and prompt 0, **without
reading the old blob**. Corrupt lengths/versions/keys and read errors therefore
cannot block explicit recovery when NVS is still writable; real write/commit
failures are returned. Bluetooth authorization still fails closed on unreadable
state. The next authorization after Automatic generates a new key. Clear Wi-Fi
preserves this namespace; factory reset erases it. Changing mode does not erase
stored server/network credentials and does not require a factory reset.

## Discovery and authentication

Maintenance keeps service `7a5e0001-7b6d-4f8b-9c2e-1d0a5a110001` and existing
setup authentication. In supported experimental builds its public info adds
`"photo":2,"family":"tesserae","kind":"<stable catalog kind>"` and diagnostics
adds `screen_mode`. On a mode read error it instead reports
`"screen_mode_error":true`, without inventing a current mode. Companion offers
an authenticated **Restore Automatic (Wi-Fi)** action in that state; Manual
remains unavailable until recovery succeeds. Fresh-device setup does not offer
the photo feature.

Photo sessions use service `7a5e0008-7b6d-4f8b-9c2e-1d0a5a110001`, protocol 2,
advertisement mode/flags byte `0x04` and the board's existing raw hardware code.
They expose the same public-info 0002, QR-control 0003, events 0005 and a new
binary-data 0006 characteristic. All UUID suffixes match the service suffix.
The 0004 native-passkey characteristic rejects photo-mode writes. Photo
authentication uses only the persisted photo key with the existing BLE v2
HKDF/AES-256-GCM envelope, fresh session ID and per-connection nonce. A photo key
is never emitted as a setup QR. Native Maintenance writes additionally check
that the connection is encrypted and authenticated.

Control and binary traffic share directional AEAD counters, while each has its
own chunk reassembler. Every queued command and receiver buffer belongs to a
connection generation. Disconnect cancels incomplete data; reconnect requires
a fresh begin. Work queued by a previous connection cannot write a later one.

Authenticated Maintenance operation:

```json
{"op":"set_screen_mode","value":"bluetooth","request":123}
{"event":"screen_mode","value":"bluetooth","request":123,"photo_key":"<32 bytes base64url>"}
```

`value:"wifi"` revokes the key; its reply omits `photo_key`. `request` must be a
nonzero UInt32. Success replies are sent only after NVS commit. Mode changes do
not reboot while the phone is still storing that reply.

After a successful mode commit, the remainder of that Maintenance session only
accepts `diagnostics`, `set_screen_mode` (retry or another mode choice) and
`reboot`. Other operations, including Wi-Fi configuration, clear-Wi-Fi and
factory reset, return an error asking the phone to close Maintenance first.
Re-enter Maintenance for those actions. This prevents a disconnect-triggered
mode reboot from replacing another action after its acceptance reply.

## Capabilities and pixel layout

In the photo session, `{"op":"photo_capabilities"}` returns the full descriptor.

The authenticated reply also includes optional `battery_mv` and `battery_pct`
telemetry, sampled when handling the request. Percentages use the same
board-specific curve or fuel gauge as network status. Unavailable readings are
null. These fields are not part of the capability digest or pixel format.

The following are the initial enabled targets, taken from the active driver's
buffer geometry, not inferred from the phone's hardware catalog:

| Experimental env | Kind | Buffer | Format | Stride | Bytes |
|---|---|---|---|---:|---:|
| `seeed-reterminal-e1004-ble-photo` | `seeed_reterminal_e1004` | 1200 × 1600 | `spectra6-msb` | 600 | 960000 |
| `seeed-reterminal-e1001-ble-photo` | `seeed_reterminal_e1001` | 800 × 480 | `mono1-msb` | 100 | 48000 |
| `seeed-reterminal-sticky-ble-photo` | `seeed_reterminal_sticky` | 480 × 800 | `gray2-msb` | 120 | 96000 |
| `seeed-reterminal-e1003-ble-photo` | `seeed_reterminal_e1003` | 1872 × 1404 | `gray4-msb` | 936 | 1314144 |

Example exact E1004 descriptor (minified size is under the 512-byte control
message limit):

```json
{"event":"photo_capabilities","schema":2,"kind":"seeed_reterminal_e1004","width":1200,"height":1600,"visible_width":1200,"visible_height":1600,"x_offset":0,"format":"spectra6-msb","stride":600,"bytes":960000,"chunk_bytes":384,"digest":"e4a1365bc0232bd2666dbd403055fa710ab6cbe60cf2eaa2805140a7ea5f0f3f","row_order":"top_down","lifecycle":"manual","max_window":4,"write_without_response":true}
```

Rows are top to bottom. The left pixel occupies the most significant bit(s) or
nibble; row stride is exactly `ceil(width*bpp/8)`. The visible rectangle is the
entire buffer on these four boards, with `x_offset=0`. Maximum frame is 4 MiB.
The format IDs reserved by the shared contract are:

| Format | Bits/pixel | Codes |
|---|---:|---|
| `mono1-msb` | 1 | 0 black, 1 white |
| `gray2-msb` | 2 | 0 black through 3 white |
| `gray4-msb` | 4 | 0 black through 15 white |
| `bwry2-msb` | 2 | 0 black, 1 white, 2 yellow, 3 red |
| `spectra6-msb` | 4 | 0 black, 1 white, 2 yellow, 3 red, 5 blue, 6 green |
| `acep7-msb` | 4 | 0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow, 6 orange |

Only the four profiled formats are enabled by this firmware iteration. Other
hardware must supply verified driver geometry/color codes and pass physical
tests before the feature gate is widened.

E1003's `gray4-msb` frame is in normal display coordinates: nibble 0 is black,
15 is white, even columns use the high nibble and odd columns the low nibble.
The unchanged IT8951 full-frame path compensates for the ED103TC2 glass's
physical X mirror inside `stream_row`: it reverses row bytes and swaps their
nibbles once, with `EPD_IT8951_MIRROR_X=1`. `ENDIAN_B` is SPI word order, not an
additional image flip. The phone encoder and BLE receiver must not pre-mirror,
rotate or swap nibbles; rows remain top to bottom. The photo path calls the same
`epd_display_frame`/full driver used by ordinary frames, without overlay or
partial refresh composition.

The E1003 experiment inherits its ordinary target's
`sdkconfig.e1003-memory.defaults` external-BSS placement, icon font, partitions,
SPI clocks, panel/rail behavior and `--no-stub` upload setting. It changes only
the photo feature and OTA capability build flags. See
[E1003 memory validation](e1003-memory-validation.md) for the established memory
and flashing constraints; this addition does not redo those hardware results.

`digest` is lowercase SHA-256 of the following canonical UTF-8 sequence with
decimal integers, pipe separators, no escaping, trailing pipe or newline:

```text
2|kind|width|height|visible_width|visible_height|x_offset|format|stride|bytes|384|top_down|manual
```

These placeholder names mean the descriptor's actual values. The phone checks
the descriptor and echoes its digest; it does not reconstruct it from a catalog.

## Transfer

The optional `max_window` extension accepts integers 1–4. The receiver
advertises `max_window:4`, tied to its four-command queue.
A client may keep up to four ordered logical packets outstanding after the
begin ACK. If the field is absent, use window 1; existing clients may continue
stop-and-wait and ignore the extra capability. This transport limit does not
change the canonical pixel descriptor/digest, encoded bytes, begin/end messages,
crypto envelope or per-packet cumulative ACK shape. It is an opt-in client
pipeline, with no new ACK batching or compression format:

```json
{"op":"photo_begin","id":123,"schema":2,"bytes":960000,"format":"spectra6-msb","capability_digest":"<descriptor digest>","sha256":"<whole frame SHA-256>"}
{"event":"photo_ack","id":123,"offset":0}
```

`id` must be a nonzero UInt32. Schema, byte count, format and descriptor digest
must match current capabilities. Send logical binary packets to characteristic
0006 as `[id:UInt32 big endian][offset:UInt32 big endian][1..384 pixel bytes]`,
inside the existing BLE v2 encrypted/chunked framing. Each receives `photo_ack`
with the next expected offset. An exact duplicate may be ACKed without
appending. A valid future-offset packet leaves the buffer unchanged and ACKs the
current offset so a dropped Write Command can be recovered; conflicting retries,
wrong IDs/generations and out-of-bounds packets still return errors. ACK offsets
are cumulative across all received packets, so a later ACK also confirms earlier
packets whose notifications were lost. Send `photo_end` only after the entire
frame is ACKed, never merely when all packets have been queued. Duplicate/stale
ACKs cannot advance the sender.

The authenticated optional `write_without_response:true` capability allows photo
data to use ATT Write Commands only if characteristic 0006 also advertises that
property. Control remains Write With Response. `chunk_bytes:384` is a receiver
maximum; choose `min(384, maximumWriteValueLength(.withoutResponse) - 33)` pixel
bytes for each connection (also for reliable writes), including the 8-byte photo
header, 4-byte fragment header and 21-byte encrypted envelope. A nonpositive
result falls back to the existing reliable-write fragmentation path. For example
MTU 256 allows 220 pixel bytes; this is an example, not a hardcoded MTU. Respect
CoreBluetooth write readiness, ordered counters and the advertised
logical-packet window. Older firmware lacks this capability and older apps may
continue their existing reliable writes.

On a Write Command timeout, stop refilling, drain the phone's local write queue,
and send `{"op":"photo_status","id":123,"request":7}` with response. Both
control and data are ordered on the same receiver worker queue; the matching
`{"event":"photo_status","id":123,"request":7,"offset":220}` is a drain barrier.
Ignore normal ACKs for refill while waiting, then discard old in-flight records
and restart the bounded window at the returned offset using fresh encrypted
frames. The status offset must be a sent packet boundary within the already sent
range; status never begins a new transfer or changes the buffer. Use a fresh
nonzero UInt32 `request` for each barrier and accept only the matching reply, so
a delayed status cannot restart a later window. Limit no-progress recovery to
two attempts. This prevents a lost command from creating a permanent trailing
gap or a late ACK from allowing old and new windows to overfill the queue.

For reliable writes, an eight-second timeout still retries only the oldest
unconfirmed logical packet with fresh AEAD counters. Its ACK reports the current
cumulative offset; do not replay the whole outstanding window. Normal progress
resets the retry budget. Disconnect discards incomplete data.

Photo events no longer incur the fixed 15 ms sleep after every notification
fragment. NimBLE enqueue congestion instead retries for at most 100 ms per
fragment on ENOMEM/EBUSY, yielding one scheduler tick and rechecking connection
generation/cancellation with fresh encryption counters; other errors stop the
event. Cumulative ACK/retry recovers a missing data ACK. Existing non-photo
notification pacing remains. This is bounded queue handling, not a measured
BLE throughput or energy result.

Photo sessions request a 15–30 ms connection interval, zero peripheral latency
and a 4-second supervision timeout using NimBLE `ble_gap_update_params`. The
central can refuse; refusal does not terminate the session. Connection/update/MTU
logs report actual interval, latency, timeout and MTU for hardware comparisons.
These preferences follow [Apple QA1931](https://developer.apple.com/library/archive/qa/qa1931/)
and the [ESP-IDF NimBLE connection API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/ble/get-started/ble-connection.html);
they do not determine or prove maximum throughput.

```json
{"op":"photo_end","id":123}
{"event":"photo_received","id":123}
```

End succeeds only after the exact size and frame SHA-256 match. `photo_received`
attests verified receipt, not physical refresh success: the panel driver returns
void. There is no partial-refresh promise. A verified frame is immutable until
painted or the session is scrubbed after painting.

`{"op":"photo_cancel","id":123}` returns `photo_cancelled`, discards the
matching incomplete buffer and does not paint. Short frames, failed hashes,
cancelled and disconnected transfers cannot paint. There is no cross-connection
resume in this version.

## Window versus packed-byte RLE

This iteration chooses window 4, preserving the receiver and all existing
wire/crypto/pixel contracts. Window 8 would exceed the current command-queue
bound; increasing it has no demonstrated benefit yet. The pipeline overlaps
application-level ACK waits; it does **not** divide the number of packets or
notifications by four, and ATT writes still await responses. A fourfold speedup
cannot be inferred.

A simple RLE alternative using `[count:1 byte (1..255), packed_byte:1 byte]`
reduces flat content substantially but can double high-entropy/dithered data.
An offline model (no compression metadata) produces:

| Input | E1004 raw/RLE bytes | E1003 raw/RLE bytes |
|---|---:|---:|
| Flat | 960000 / 7530 | 1314144 / 10308 |
| Existing palette bars | 960000 / 19200 | 1314144 / 67392 |
| Deterministic valid-palette high-entropy surrogate | 960000 / 1866490 | 1314144 / 2618228 |

These surrogates are not real photos. RLE is deferred: it needs explicit
encoding negotiation, encoded versus decoded size/offset rules, bounded
streaming decode, original-frame hashing, raw fallback and new adversarial
coverage. Its additional protocol/code surface is unnecessary for the first
window acceptance pass. A future RLE extension must never silently replace raw
frames. Reproduce the counts with:

```sh
python3 tools/generate_ble_photo_fixtures.py /tmp/photo-v2-fixtures
python3 tools/analyze_ble_photo_transport.py /tmp/photo-v2-fixtures
```

E1004 still has 2500 logical data packets and E1003 3423. In the old code, the
fixed 15 ms delay alone accumulated to 37.500 s and 51.345 s respectively
(excluding begin/end and fragmentation). Those are static lower-bound delay
counts, **not measured transfer times**. Real elapsed time depends on MTU,
connection interval, ATT writes, RSSI, phone scheduling, retries and panel work.

## Reproducible validation

From the repository root:

```sh
sh tools/test_ble_photo.sh
sh tools/test_ble_setup_protocol.sh
python3 tools/generate_ble_photo_fixtures.py /tmp/photo-v2-fixtures
pio run -e seeed-reterminal-e1003-ble-photo -e seeed-reterminal-e1004-ble-photo -e seeed-reterminal-e1001-ble-photo -e seeed-reterminal-sticky-ble-photo
pio run -e seeed-reterminal-e1003 -e seeed-reterminal-e1004 -e waveshare-133e6
```

The photo script explicitly resolves the project-pinned ESP-IDF package first;
it does not require a prior firmware build. Host scripts use its mbedTLS under
`${PLATFORMIO_CORE_DIR:-$HOME/.platformio}`. Receiver/mode tests run with Address
and Undefined Behavior Sanitizers. They cover retransmissions, wrong offsets and
IDs, a four-packet window with lost ACKs and an oldest-packet retry,
cancellation, bad hashes, short/oversized frames, immutable commit, generation
isolation, a complete 4 MiB frame, NVS open/set/commit failures, one-shot prompt
consumption, v1 migration, key reuse/revocation, corrupt/unreadable configuration
recovery without reads, and the post-mode-commit command gate.
Existing maintenance gesture and BLE
crypto vectors remain covered.

Small descriptor/hash goldens are in `test/fixtures/ble_photo`; the generator
recreates raw bars without storing large binaries in Git. E1004 bars are black,
white, yellow, red, blue and green from left to right, repeated on every row.
E1001 is black/white halves; Sticky is four increasing gray levels.
E1003 has sixteen left-to-right levels, 117 pixels per bar, on all 1404 rows.
Its odd-width bars deliberately cross nibble boundaries: pixels 116 and 117
pack as `0x01`, not `0x10`; the first/last pixels are black/white. The frame
SHA-256 is `32bd2387c1725cfecc1752e8a2e8d77b32e02cf1ffca725b5e9043e587548bfb`;
the descriptor digest is
`324dcf85431b812a6c469bd3649552b41533155ddfb2bb5f7561d5d12f7d82f6`.
To validate an independently encoded phone frame through the actual C receiver and real
mbedTLS SHA implementation, run:

```sh
sh tools/test_ble_photo.sh /absolute/path/frame.bin <64-character SHA-256>
```

To keep a standalone receiver executable for cross-language tests (run from the
repository root):

```sh
photo_mbedtls="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-espidf/components/mbedtls/mbedtls"
cc -std=c11 -Wall -Wextra -Werror \
  -DMBEDTLS_CONFIG_FILE='"relay_mbedtls_config.h"' \
  -I src -I test -I "$photo_mbedtls/include" \
  test/test_ble_photo_fixture.c src/ble_photo.c \
  "$photo_mbedtls/library/sha256.c" \
  "$photo_mbedtls/library/platform_util.c" \
  "$photo_mbedtls/library/constant_time.c" \
  -o /tmp/tesserae-ble-photo-receive-fixture
python3 tools/generate_ble_photo_fixtures.py /tmp/photo-v2-fixtures
/tmp/tesserae-ble-photo-receive-fixture /tmp/photo-v2-fixtures/e1004.bin "$(cat /tmp/photo-v2-fixtures/e1004.sha256)"
```

An independent Swift E1004 encoder run has produced 960000 bytes, matched a
Python per-byte color oracle, and passed this C receiver in 384-byte chunks.
That cross-language frame SHA-256 was
`7e88b3db01f7e8c6b66ad4111ca47e48c891a5ff785e07fe33d3566455af2404`.
This validates byte layout, not BLE throughput or physical color output.

E1003 additionally passed a 1,314,144-byte independent Swift encode of
`(x/117 + y/78) % 16` gray levels, a full Python per-byte oracle and the C
receiver with real mbedTLS SHA. It used 3423 logical packets, including the
final 96-byte packet. That cross-language frame SHA-256 was
`c1a3309bcaf1663b33c035132dfcf44faf61c154fca304f037b1c2444fb9de1b`.
This pattern checks both axes and nibble order without pre-mirroring the image.

Initial validation on 2026-10-21 used baseline
`421d51a41a843e94757b2261aed619aad0996b66` (1.52.0), ESP-IDF 5.5.2 and
pioarduino 55.03.37. The E1003/OTA-isolation follow-up builds on `1c45cef`
(the first Photo v2 implementation). All seven envs in the commands above
built successfully: four opt-in experimental targets, normal E1003/E1004 and
BLE-disabled `waveshare-133e6`. Both host scripts and all four frame fixtures
passed. The build host needed Python 3.12 and PlatformIO 6.1.19; its older
Python 3.9 installation could not run this platform. A task-local PlatformIO
core reused the installed package cache without replacing the user's Python.

Resolved PlatformIO configuration checks confirmed identical board, SDK
defaults, partitions and embedded fonts between each normal/experimental pair,
except the intended feature flags. Generated E1003 sdkconfig retained external
BSS and NimBLE. Symbol inspection of the linked experimental images found no
OTA installer/capability functions; normal E1003/E1004 retained both. This
confirms OTA isolation beyond relying on manual mode's lack of network polls.

| Successful env | Static RAM bytes | Flash bytes |
|---|---:|---:|
| `seeed-reterminal-e1003-ble-photo` | 97628 | 2218693 |
| `seeed-reterminal-e1004-ble-photo` | 92636 | 1613061 |
| `seeed-reterminal-e1001-ble-photo` | 92652 | 1615161 |
| `seeed-reterminal-sticky-ble-photo` | 182828 | 2205061 |
| `seeed-reterminal-e1003` (normal BLE) | 96492 | 2237225 |
| `seeed-reterminal-e1004` (normal BLE) | 91508 | 1632893 |
| `waveshare-133e6` (BLE off) | 85892 | 1390389 |

These linker totals exclude dynamic photo buffers and do not establish runtime
heap headroom. Local build log: `/tmp/tesserae-e1003-ble-photo-final-builds.log`;
host/cross-language log: `/tmp/tesserae-e1003-ble-photo-host-tests.log`. Each
binary is at `.pio/build/<env>/firmware.bin`; build and hardware results are
separate acceptance steps.

## Hardware acceptance still required

- Run the complete Maintenance → Bluetooth authorization → disconnect/reboot →
  short-button photo flow on all four boards, using QR and passkey for
  Maintenance. Confirm a cold photo session needs neither Wi-Fi nor server.
- Paint color/gray/mono bars and an asymmetric orientation/corner test. Confirm
  geometry, row direction, color codes, black/white polarity and no hidden-column
  shift. Measure full refresh time and inspect the glass after the final ACK.
- Interrupt each transfer stage, reconnect, send a different image, inject bad
  hashes/conflicting retries, cancel and let deadlines expire. Confirm only a
  verified end can change the panel, including immediate final-ACK disconnect.
- Verify manual mode across timer wakes and power cycles with the configured
  server changing frames. Confirm Wi-Fi/BLE are off during sleep, verify button
  maintenance/reset gestures, then switch to Automatic and observe a fresh
  server frame. After saving a mode, request clear-Wi-Fi/factory-reset before
  closing: both must return an error rather than an acceptance reply. Re-enter
  Maintenance and confirm those actions work and retain/revoke the photo key
  as specified.
- With a known photo visible, power-cycle/reset after the one-shot entry prompt
  was consumed; induce watchdog/brownout only with appropriate lab controls.
  Confirm the instruction is not repainted. Reauthorize the same Manual mode
  and check that it does not re-arm the prompt. A deliberate Automatic → Manual
  transition should paint the instruction once. In Maintenance, corrupt the
  mode blob, confirm explicit read-error diagnostics, then restore Automatic
  without erasing network/server credentials; verify actual NVS write failures
  remain errors. Host fault injection does not prove flash power-loss behavior.
- Compare reliable writes/window 1 (older Companion) and negotiated Write
  Commands/window 4 with identical frames, MTU, RSSI and connection conditions;
  record transfer-only time, retries, refresh time and current separately. No
  physical speed or power result is available.
- Test low battery, charging/recovery, practical 960 KB/1.31 MB throughput, MTU/packet
  fragmentation, RSSI/retry behavior, panel refresh current and asleep current.
- On E1003, check the asymmetric nibble/orientation pattern and memory headroom
  during the 1,314,144-byte receive and subsequent IT8951 paint. Offer a normal
  server OTA while in Automatic on the experimental build: it must neither
  advertise OTA capability nor install that image.

Normal release envs do not enable this feature and must continue their existing
setup/maintenance and automatic transport behavior. There is no flashing,
hardware validation, OTA publication or release approval implied by these tests.
