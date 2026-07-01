# tesserae-device-esp32-bin

Battery-powered ESP32-S3 firmware that's the embedded client for the [Tesserae](https://github.com/dmellok/tesserae) server — drives a [Waveshare ESP32-S3-ePaper-13.3E6](https://www.waveshare.com/esp32-s3-epaper-13.3e6.htm) (13.3", 1200×1600, 6-colour Spectra E6 e-paper panel on an ESP32-S3-WROOM-2-N32R16V module).

The device wakes on a timer, publishes a heartbeat with its battery state, pulls a retained MQTT message containing a `.bin` frame URL, downloads the panel-native 4-bpp buffer, paints the panel, and goes back to deep sleep. WiFi credentials and MQTT broker details are provisioned on first boot via a SoftAP captive portal.

Tesserae's `esp32_bin` renderer composes a frame, packs it into the panel-native 4 bpp `.bin` format, and publishes the URL **retained** on `tesserae/<device_id>/frame/bin` so a battery client can connect briefly, immediately receive the most recent frame URL (no waiting), and sleep again.

**Multiple panels:** each device has a `device_id` (default `esp32`) that prefixes its MQTT topics, so several ESP32 panels can share one broker and one Tesserae server. Set it from the setup portal or bake it in at compile time with `MQTT_DEFAULT_DEVICE_ID` in `secrets.h` — see [Provisioning & settings](#provisioning--settings).

## Why retained MQTT + URL hash + WiFi-before-paint

The whole architecture is in service of one constraint: **months of battery life from a single Li-Po pouch cell**. Every design choice falls out of that:

- **Retained MQTT** — no polling, no holding a connection. The client connects, the broker hands it the latest URL within hundreds of ms, the client disconnects.
- **URL hash skip** — the SHA-256 of the last rendered URL lives in NVS. If the URL hasn't changed, we skip the ~30 s panel refresh on this wake. That's ~0.6 mAh saved per hash-skip wake.
- **WiFi off before painting** — `wifi_sta_stop()` (and the MQTT client) are torn down *before* `epd_init()` so we don't burn ~80 mA holding the radio during the multi-second panel render. This is the single biggest battery saving in the render path.
- **Panel power gated by GPIO** — `EPD_PIN_PWR` (GPIO1) is high only during refresh; `epd_sleep()` drops it.

## Hardware

| Component | Detail |
| --- | --- |
| Module | ESP32-S3-WROOM-2-N32R16V (32 MB flash, 16 MB octal PSRAM) |
| Panel | 13.3" Spectra E6, 1200×1600 native portrait, 6 colours, 4 bpp packed |
| Battery | Single-cell Li-Po (3.3–4.2 V) via the onboard ETA6098 charger; battery sense on GPIO8 / ADC1 ch7 through a 1:3 divider |

Pinout (see [include/app_config.h](include/app_config.h)) matches the official Waveshare ESP-IDF demo. The panel has two SPI chip-selects (`CS_M`, `CS_S`) which drive the left and right halves of the display independently.

### Power profile

| State | Current | Duration |
|---|---|---|
| WiFi + MQTT + HTTP burst | ~80 mA | 5–15 s |
| Panel refresh | ~60 mA | ~30 s (only on render wake) |
| Deep sleep | single-digit µA | the rest of the time |

Per-wake cost on a 4 V Li-Po, default 15 min sleep interval:

- **Hash-skip wake** (no render): ~0.13 mAh
- **Render wake** (download + paint): ~0.75 mAh
- **Deep sleep idle**: ~1.2 mAh/day

Expected runtime by usage:

| Use case | Daily draw | 2000 mAh | 5000 mAh | 10000 mAh |
|---|---|---|---|---|
| Photo frame (1 update/day) | ~14 mAh | 5 months | **12 months** | 24 months |
| Dashboard (hourly updates) | ~30 mAh | 2 months | **5.5 months** | 11 months |
| Frequent (every wake renders) | ~75 mAh | 1 month | 2 months | 4 months |

For "months of usage" with a typical update cadence, a 5000–10000 mAh single-cell Li-Po pouch is the sweet spot. Bumping `sleep_interval_s` via the config topic from 15 min to 30 or 60 min proportionally cuts wake-cycle cost.

## Wake cycle

```
boot
  ├─ double-tap RESET?               → serve LAN settings editor + mDNS → sleep/reboot
  ├─ cold boot + WiFi creds present? → paint logo splash (~30 s)
  ├─ no WiFi creds anywhere?         → paint portal splash (logo + WPA QR) → captive portal → reboot
  ├─ STA connect fails?              → paint portal splash → captive portal → reboot
  ├─ STA connect
  ├─ NTP sync (cold boot only; RTC carries time across deep sleep)
  ├─ fetch retained frame URL        (no heartbeat yet)
  ├─ no URL? / URL unchanged?        → publish heartbeat → sleep
  ├─ render path: download → free WiFi → paint (~30 s)
  ├─ reconnect WiFi briefly          (~3-5 s, for the post-paint heartbeat)
  ├─ publish heartbeat               (sleep_until = now + sleep_s, smart-sync feed)
  └─ deep sleep OR loop:
       ├─ USB host attached?         → short-delay restart loop (dev mode)
       └─ otherwise                  → deep sleep for the configured interval
```

The heartbeat lands at the END of each wake (post-paint on render wakes), so `sleep_until` reflects the actual sleep about to start — Tesserae's smart-sync scheduler uses it to JIT-render the next frame so the device sees a fresh artifact the moment it wakes.

The "no sleep when USB-host attached" logic uses ESP-IDF's `usb_serial_jtag_is_connected()` to read host SOF packets — a USB charger / power bank (no SOFs) is treated as battery operation. Force-override either way with `DEV_DISABLE_SLEEP` in `secrets.h`.

The cold-boot splash fires only on `ESP_RST_POWERON` / `ESP_RST_EXT` — burning 30 s on every timer wake would halve battery life.

Behaviour tunables (sleep interval, retry counts, AP credentials, MQTT topics / broker URI) all live at the top of [include/app_config.h](include/app_config.h).

## Build & flash

Requires [PlatformIO](https://platformio.org/). ESP-IDF 5.x and the Xtensa toolchain are pulled automatically on first build.

```bash
pio run                                          # build
pio run -e tesserae-device-esp32-bin -t upload   # flash via USB
pio device monitor                               # 115200 baud, exception decoder enabled
```

First boot brings up a SoftAP named `Tesserae-Setup` (password `tesserae`). Join it from your phone; the captive-portal prompt opens a form for your home WiFi credentials and MQTT broker. After submit the device reboots, joins your network, and enters the normal wake cycle.

### Dev shortcut: `secrets.h`

To skip the captive portal during iteration, copy [include/secrets.example.h](include/secrets.example.h) to `include/secrets.h` and uncomment the `WIFI_DEFAULT_*` / `MQTT_DEFAULT_*` macros you want baked into the build. `secrets.h` is git-ignored. Precedence on each wake is NVS (set via portal) → `secrets.h` values → empty (portal triggers).

For fast iteration without USB plugged in (e.g. headless testing), also define `DEV_DISABLE_SLEEP` in `secrets.h` — the firmware will loop on `DEV_LOOP_INTERVAL_S` (default 10 s) instead of deep-sleeping. With USB plugged in this is automatic; the manual flag is only needed otherwise.

## Flashing a pre-built release

Each release tag on GitHub ships four artifacts plus a checksum file:

```
bootloader.bin         second-stage bootloader (esp-idf)
partitions.bin         partition table (matches partitions.csv)
firmware.bin           the application image
firmware.factory.bin   combined image (bootloader + partitions + firmware at offset 0)
SHA256SUMS             SHA-256 of each .bin above
```

Verify the downloads before flashing:

```bash
shasum -a 256 -c SHA256SUMS
```

The simplest flash uses the combined image written to offset 0:

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \
    write_flash 0x0 firmware.factory.bin
```

Or flash the three pieces individually at their native offsets (matches what PlatformIO does during `pio run -t upload`):

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem... \
    write_flash 0x0     bootloader.bin \
                0x8000  partitions.bin \
                0x10000 firmware.bin
```

**Finding the port.** The Waveshare board exposes two USB devices when plugged into a laptop: the ESP32-S3 native USB-JTAG/Serial (`303A:1001`) and an onboard CH343 UART (`1A86:55D3`). Flash through the CH343 — that port drives DTR/RTS for the auto-reset circuit. On macOS it shows up as `/dev/cu.usbmodem<serial>`; on Linux as `/dev/ttyACM<n>` or `/dev/ttyUSB<n>`. `pio device list` lists candidates with VID/PID.

**First boot after flashing.** The cold-boot path paints the portal splash (~30 s) and brings up the `Tesserae-Setup` SoftAP (password `tesserae`). Scan the QR on the splash with a phone to join, then fill in your WiFi + MQTT broker in the captive portal.

**Erase NVS first** if you want a clean state on a previously-provisioned board (otherwise stored WiFi creds + MQTT URI + `device_id` survive):

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem... erase_flash
# then write_flash as above
```

The release-build pipeline lives at [`tools/release.sh`](tools/release.sh) — it rebuilds the current `FW_VERSION` from a clean tree, stages artifacts under `release/<version>/`, computes SHA-256, tags `vX.Y.Z`, and creates a GitHub Release with the artifacts attached.

## Boot splashes

Two panel-native splashes are baked into the firmware image (built from PNG via [`tools/gen_splash.py`](tools/gen_splash.py), Floyd-Steinberg dithered to the 6-colour panel palette, and embedded as 960,000-byte blobs via CMake's `EMBED_FILES`):

- **Logo splash** ([`assets/splash_logo.bin`](assets/splash_logo.bin)) — Tesserae logo centered on white. Painted on cold-boot (`ESP_RST_POWERON` / `ESP_RST_EXT`) when WiFi creds are present. Skipped on timer-wake (every normal sleep cycle would burn 30 s of refresh power for nothing) and in settings mode.
- **Portal splash** ([`assets/splash_portal.bin`](assets/splash_portal.bin)) — logo at top, a baked WPA-format QR code below: `WIFI:T:WPA;S:Tesserae-Setup;P:tesserae;;`. Scan with a phone to join the SoftAP without typing. Painted whenever the firmware is about to bring up the captive portal (no creds, or STA connect failed) — first-boot users see it on the panel and can join the AP without ever opening secrets.

To regenerate after editing the logo or changing `PROVISION_AP_SSID` / `PROVISION_AP_PASS` in `include/app_config.h`:

```bash
LOGO=/path/to/tesserae-splash-1024.png
python3 tools/gen_splash.py --logo "$LOGO" --out assets/splash_logo.bin \
    --logo-size 600
python3 tools/gen_splash.py --logo "$LOGO" --out assets/splash_portal.bin \
    --logo-size 600 --logo-y 100 \
    --qr-data 'WIFI:T:WPA;S:Tesserae-Setup;P:tesserae;;' \
    --qr-size 700 --qr-y 850
```

Both files are committed; the build embeds them and they make up ~2 MB of the flashed firmware image. The dither is pure Python (no native deps beyond Pillow + numpy + qrcode); each splash takes ~15 s on a laptop.

## Provisioning & settings

All settings (WiFi SSID + password, MQTT broker URI + credentials, and `device_id`) are entered through the same HTML form, reachable two ways:

**First-boot captive portal.** With no WiFi creds in NVS (fresh flash, or after `idf.py erase-flash`), the device brings up a SoftAP named `Tesserae-Setup` (password `tesserae`). Join it from a phone; the captive-portal prompt opens the form automatically. After submit the device reboots and joins your network.

The portal has a **15-minute idle timeout** — the timer resets each time a phone associates with the AP, so a user actively in the form never times out, but an abandoned AP eventually shuts down. On expiry the device **deep-sleeps with no wakeup source configured**; only a RESET button press (chip EN line) brings it back. That avoids the alternative of retrying every 15 min on battery and flattening the cell while no one's around to set it up.

**Always-on settings editor (double-tap RESET).** Once the device is on the LAN you can re-open the form without erasing NVS: **tap the RESET button twice in quick succession** (the second tap within the first wake's few-second window). Instead of painting, the device serves the form on its STA IP and advertises it over mDNS at:

```
http://tesserae-<device_id>.local/
```

The form pre-fills the current SSID, broker URI, `device_id`, and username (passwords are never echoed — **leave a password field blank to keep the stored one**). Saving writes NVS and reboots to apply. The editor stays up for `PROVISION_PORTAL_TIMEOUT_S` (10 min) then deep-sleeps if you don't submit.

> The double-tap trigger relies on the RESET button preserving RTC memory across resets, which is board-specific. If double-tap never opens the editor on your board, fall back to the captive portal (`idf.py erase-flash` then re-provision). The settings server has **no authentication** — it trusts the LAN; don't expose the device's IP beyond a network you control.

**`device_id`** must be 2–32 chars: lowercase letters, digits, `-` or `_`, starting with a letter (same rule Tesserae enforces in `device.json`). It defaults to `esp32`, which matches Tesserae's built-in `esp32_client` kind. Changing it re-points every topic to `tesserae/<new_id>/…`.

**Legacy migration.** Boards upgraded from the pre-multi-head firmware stored an `mqtt/topic` NVS key. On the first boot after upgrade the firmware extracts `<X>` from a `tesserae/<X>/frame/bin` value into the new `device_id` key and erases the old one (logged as `device_id initialized to '…' (migrated from legacy topic)`). Anything unrecognized — including the very old `inky/esp32/update` — falls back to `device_id=esp32`, which resolves to the same effective topics.

## MQTT contract

The firmware uses three topics under the `tesserae/<device_id>/` namespace (default `device_id` = `esp32`). The frame and config topics are read on every wake; the status topic is written on every wake.

| Topic | Direction | Retained | QoS | Purpose |
|---|---|---|---|---|
| `tesserae/<device_id>/frame/bin` | server → device | yes | 1 | URL of the next `.bin` frame to render |
| `tesserae/<device_id>/config` | server → device | yes | 1 | Runtime device settings (sleep interval) |
| `tesserae/<device_id>/status` | device → broker | yes | 1 | Wake-time heartbeat + LWT |

All three topics are derived from `device_id` at runtime. Set `device_id` via the setup portal (stored in NVS) or bake a compile-time default with `MQTT_DEFAULT_DEVICE_ID` in `secrets.h`.

The client also registers a **last-will-and-testament** on `tesserae/<device_id>/status` with `{"state":"offline"}` retained. The broker publishes that on ungraceful disconnect (keepalive timeout, TCP drop) so Tesserae can flag a probably-dead-battery device. On the next normal wake the full heartbeat overwrites the offline marker.

### Frame payload

JSON pointing to a panel-native `.bin` artifact:

```json
{ "url": "http://192.168.1.10:8000/renders/3f7a91b2c4e5d6f8.bin" }
```

A bare URL string (anything starting with `http://` or `https://`) is also accepted — useful for testing with `mosquitto_pub` directly.

**Wire format** of the file at that URL: raw, headerless, no magic bytes, no length prefix, no checksum. Exactly **960,000 bytes** (`1200 × 1600 / 2`), scanline order, no row padding, two pixels per byte where the **high nibble holds the even column** (cols 0, 2, 4 …) and the low nibble the odd column.

Palette nibble values (firmware-reserved `0x4` and `0x7` are never written):

| Nibble | Colour |
|---|---|
| `0x0` | Black |
| `0x1` | White |
| `0x2` | Yellow |
| `0x3` | Red |
| `0x5` | Blue |
| `0x6` | Green |

The buffer is panel-native — no rotation, no decode, no resize required. The left half of each row (300 bytes, columns 0–599) is streamed to the left controller (`CS_M`); the right half (columns 600–1199) to the right controller (`CS_S`) — see `epd_display` in [src/epd_driver.c](src/epd_driver.c).

If the downloaded body isn't exactly 960,000 bytes the firmware logs a `frame size mismatch` error and goes back to sleep without painting. The Tesserae server's `esp32_bin` renderer always produces this exact length; anything else is a server-side bug and feeding the panel garbage costs ~30 s of refresh power for nothing.

The firmware persists the SHA-256 of the rendered URL in NVS so unchanged retained messages don't trigger needless panel refreshes.

### Config payload

JSON; applied on next wake and persisted to NVS so it survives reboots.

```json
{ "sleep_interval_s": 900 }
```

Clamped to `[30, 604800]` (30 s – 7 days). Out-of-range values are rejected with a log warning and the previously-stored interval is kept. Falls back to the compile-time `SLEEP_INTERVAL_S` (15 min) when NVS is empty.

### Status (heartbeat) payload

Published once per wake at the **end** of the wake cycle (after the panel paint on render wakes, after the URL fetch on hash-skip wakes) so `sleep_until` reflects the actual sleep about to start. The render-path WiFi reconnect for this publish costs ~0.07–0.11 mAh per render wake — the price of accurate smart-sync predictions:

```json
{
  "battery_mv": 3950,
  "battery_pct": 67,
  "rssi": -42,
  "ip": "192.168.50.234",
  "fw_version": "0.6.0",
  "kind": "esp32_client",
  "panel_w": 1200,
  "panel_h": 1600,
  "sleep_interval_s": 900,
  "next_sleep_s": 900,
  "wake_reason": "timer",
  "sleep_until": 1759264800
}
```

- `battery_mv` — millivolts at the cell. `0` means the ADC read failed (treat as unknown, not flat).
- `battery_pct` — 0–100, derived from a two-segment piecewise-linear Li-Po curve.
- `rssi` — wifi signal in dBm at the time of the heartbeat.
- `ip` — STA IPv4 address.
- `fw_version` — firmware version string (set via `build_flags` in `platformio.ini`).
- `kind` — always `"esp32_client"`; lets Tesserae pre-fill the kind when registering a discovered device.
- `panel_w` / `panel_h` — panel pixel dimensions (`EPD_WIDTH` / `EPD_HEIGHT`), so the Register form pre-fills the panel size.
- `sleep_interval_s` — the device's *configured* deep-sleep duration. Diagnostic.
- `next_sleep_s` — the deep-sleep duration the device is **about to enter** (this firmware always sleeps for the configured interval, so it mirrors `sleep_interval_s`; a future firmware that adjusts cadence on low battery could differ).
- `wake_reason` — short string from `esp_reset_reason()`: `timer` (normal deep-sleep wake), `poweron`, `ext`, `sw`, `brownout`, `panic`, `int_wdt` / `task_wdt` / `wdt`, etc. On battery anything other than `timer` (or the first `poweron`) is a diagnostic flag — `brownout` repeated = low cell voltage under load, `panic`/`wdt` = firmware crash.
- `sleep_until` — absolute unix timestamp (UTC seconds) the device intends to wake. **Omitted** when the firmware doesn't have wall-clock time yet (NTP failed on the first cold boot, broker / WiFi blocked outbound `pool.ntp.org`). The server falls back to `heartbeat_receive_time + next_sleep_s` when absent. The RTC carries time across deep sleep so timer wakes don't re-sync, but cold boots (POWERON / EXT reset) **always** force a fresh sync — that gives a recovery path if a previous sync ever landed badly (router intercepting NTP, transient pool weirdness): power-cycle or hit RESET. Before publishing, the firmware also cross-checks `(sleep_until - now) ≈ next_sleep_s` and omits the key if the math disagrees by more than ±5 s.

Together `next_sleep_s` and `sleep_until` feed Tesserae's [smart-sync scheduler](https://github.com/dmellok/tesserae/issues/10) — after 3 on-time wakes the device flips to "trusted" and the server JIT-renders the next frame so it lands fresh right before the device wakes. To hit this contract the firmware publishes the heartbeat **at the end of each wake** (after the paint), not at the start, which costs an extra ~3-5 s of WiFi reconnect on render wakes (~0.07-0.11 mAh).

Retained, so Tesserae can show "last known state" without the device being awake. The `kind`/`panel_*` keys feed Tesserae's **Discovered devices** strip: an unregistered `device_id` heartbeating on `tesserae/+/status` shows up with a one-click Register button, pre-populated from these fields. The Last Will (`{"state":"offline"}`, **non-retained**) lets live subscribers see ungraceful disconnects without ever overwriting the retained heartbeat — combine with `sleep_interval_s` and the heartbeat timestamp to distinguish "asleep on schedule" from "actually dead".

### Manual test push

```bash
mosquitto_pub -t tesserae/esp32/frame/bin -r \
  -m '{"url":"http://192.168.1.10:8000/renders/test.bin"}'
```

(Replace `esp32` with your `device_id` if you changed it.) The next wake will fetch and paint that frame.

## Troubleshooting

- **No captive portal on first boot** — the captive portal triggers when WiFi creds aren't present in NVS, so a fresh board (or one wiped with `idf.py erase-flash`) brings up the `Tesserae-Setup` AP (password `tesserae`) automatically. To re-open settings on an already-provisioned board, double-tap RESET (see [Provisioning & settings](#provisioning--settings)).
- **STA connects but no paint** — broker unreachable from the ESP32's IP. Check `ip` in the latest retained heartbeat on `tesserae/<device_id>/status` and ping the broker URI from that subnet. Common causes: broker bound to `127.0.0.1`, firewall, VLAN isolation.
- **Paint starts and stops** — `frame size mismatch` in the serial log means the URL served something that isn't 960,000 bytes. Verify with `curl -sI <url>` that `Content-Length: 960000`.
- **Splash colours look wrong** — panel ribbon seated badly or wrong panel rev. The init byte sequence in [src/epd_driver.c](src/epd_driver.c) is panel-specific and must not be modified.
- **Battery drains in days, not months** — usually means WiFi is being held during the panel refresh. Confirm `wifi_sta_stop()` runs *before* `epd_init()` in [src/main.c](src/main.c). On a USB power meter, the 30 s refresh phase should sit around 60 mA, not 140 mA.

## Project layout

```
tesserae-device-esp32-bin/
├── platformio.ini             # board, partitions, monitor, FW_VERSION build flag
├── partitions.csv             # 14 MB factory app + NVS
├── sdkconfig.defaults         # PSRAM, mbedTLS cert bundle, MQTT 3.1.1
├── include/
│   ├── app_config.h           # pinout + all behaviour tunables
│   ├── secrets.example.h      # template for local credential overrides
│   └── secrets.h              # (git-ignored) your local overrides
└── src/
    ├── main.c                 # boot → (double-tap?) settings → splash → connect → render → sleep
    ├── idf_component.yml       # managed deps (espressif/mdns)
    ├── epd_driver.{c,h}       # Waveshare 13.3E6 panel driver + colour-bar splash
    ├── heartbeat.{c,h}        # battery / RSSI / IP / fw_version / kind / panel size JSON
    ├── wifi_manager.{c,h}     # NVS-backed STA connect + STA-IP / SSID accessors
    ├── provisioning.{c,h}     # captive portal + always-on LAN settings editor + mDNS
    ├── mqtt_config.{c,h}      # NVS-backed broker URI / device_id / credentials + migration
    ├── mqtt_handler.{c,h}     # single-shot subscribe + dispatch + heartbeat + LWT
    ├── image_fetcher.{c,h}    # HTTP download into PSRAM
    └── image_decoder.{c,h}    # strict 960000-byte panel-native pass-through
```

No tests — smoke-testing on real hardware is the workflow. Recommended validation after any change to the wake state machine:

1. Flash a fresh board, walk it through the captive portal.
2. `mosquitto_pub -t tesserae/esp32/frame/bin -r -m '{"url":"http://.../test.bin"}'` and confirm the panel paints.
3. Double-tap RESET and confirm `http://tesserae-esp32.local/` (or the device IP) serves the settings form pre-filled with live values.
4. Pull battery, attach to a USB current meter, and log average draw over 24 h. Anything above ~35 mAh/day for the photo-frame use case means WiFi is leaking on somewhere.

## Credits

The panel driver in [src/epd_driver.c](src/epd_driver.c) is a port of the official ESP-IDF demo published by Waveshare at [waveshareteam/ESP32-S3-ePaper-13.3E6](https://github.com/waveshareteam/ESP32-S3-ePaper-13.3E6). The init byte sequence and command set are panel-specific and kept byte-for-byte exact.

The wake state machine, captive-portal provisioning, NVS schema, and battery curve are forked from the earlier `esp32-inky-dash-client` project; the same hardware substrate runs both.

## License


AGPL-3.0-or-later. See [LICENSE](LICENSE).

