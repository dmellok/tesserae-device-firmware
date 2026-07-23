# tesserae-device-firmware

One **ESP-IDF** firmware that drives **many e-paper devices from a single
codebase**, part of the [Tesserae](https://github.com/dmellok) self-hosted
e-ink dashboard ecosystem. It is a battery-first, deep-sleep client: each wake
it connects WiFi, asks a Tesserae server over REST for the current frame,
downloads a **panel-native** buffer, paints it, and goes back to sleep.

The device is a deliberately thin client — it does **no on-device image
decoding**. The server pre-renders each frame into exactly the byte format the
panel wants (packed 4-bit Spectra-6, 1-bit mono, or 4-bit grayscale); the device
streams those bytes straight to the panel. Adding a new panel is a board header
plus one driver, with the shared network / cycle / splash / power stack
untouched.

## Supported devices

Every panel is driven through the same `epd_driver_t` vtable; the build selects
one board (and thus one driver) per PlatformIO environment.

| Device | Panel family | Controller | Resolution / depth | Driver | Env |
| --- | --- | --- | --- | --- | --- |
| [Seeed reTerminal **E1001**](https://www.seeedstudio.com/reTerminal-E1001-p-6534.html) | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `seeed-reterminal-e1001` |
| [Seeed reTerminal **E1002**](https://www.seeedstudio.com/reTerminal-E1002-p-6533.html) | Spectra-6, single | UC81xx | 800×480, 4bpp | `spectra6_spi_single` | `seeed-reterminal-e1002` |
| [Seeed reTerminal **E1003**](https://www.seeedstudio.com/reTerminal-E1003-p-6731.html) | Grayscale (10.3") | IT8951 | 1872×1404, 4bpp gray | `it8951_gray` | `seeed-reterminal-e1003` |
| [Seeed reTerminal **E1004**](https://www.seeedstudio.com/reTerminal-E1004-p-6692.html) | Spectra-6, dual-chip | T133A01 | 1200×1600, 4bpp | `spectra6_t133a01_dual` | `seeed-reterminal-e1004` |
| [Seeed **XIAO ePaper Kit — EE02**](https://www.seeedstudio.com/XIAO-ePaper-DIY-Kit-EE02-for-13-3-Spectratm-6-E-Ink.html) | Spectra-6, dual-chip | T133A01 | 1200×1600, 4bpp | `spectra6_t133a01_dual` | `seeed-ee02` |
| [**TRMNL 7.5" OG DIY Kit**](https://www.seeedstudio.com/TRMNL-7-5-Inch-OG-DIY-Kit-p-6481.html) | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `xiao-epaper-75` |
| [Seeed **XIAO ePaper Display Board — EE04**](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-EE04-p-6560.html) + 7.5" mono (24-pin) | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `seeed-ee04-75` |
| [Seeed **XIAO ePaper Display Board — EE04**](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-EE04-p-6560.html) + 7.3" Spectra-6 (50-pin) | Spectra-6, single | UC81xx | 800×480, 4bpp | `spectra6_spi_single` | `seeed-ee04-73e6` |
| [Waveshare **ESP32-S3-ePaper-13.3E6**](https://www.waveshare.com/esp32-s3-epaper-13.3e6.htm) | Spectra-6, dual-controller | UC81xx ×2 | 1200×1600, 4bpp | `spectra6_spi_dual` | `waveshare-133e6` |
| [Waveshare **PhotoPainter 7.3"**](https://www.waveshare.com/esp32-s3-photopainter.htm) | Spectra-6, single | ED2208-GCA | 800×480, 4bpp | `spectra6_spi_single` | `waveshare-photopainter-73` |

The four reTerminals, the PhotoPainter, the EE02, and the TRMNL 7.5" kit have been
verified end-to-end on real hardware; the Waveshare 13.3E6 is the seed target and
builds green. The EE04 pair builds green but is **not yet hardware-verified**
(pin map taken from Seeed_GFX; the EE04 takes one panel on either its 24-pin or
50-pin FPC — flash the env matching the attached panel and set the jumper caps
accordingly). Each board also has a `…-selftest` env that paints a driver-only
test pattern (colour bars / gray ramp / mono stripes) with no networking — flash
that first when bringing up a new unit.

Reuse is the norm — most boards share an existing driver and differ only in the
board header:

- The **PhotoPainter** shares the E1002's `spectra6_spi_single` driver (its
  ED2208-GCA init is byte-identical); two board flags tailor it: `EPD_ROTATE_180`
  (panel mounted upside-down) and `BOARD_HAS_PMIC` (panel power + battery from an
  **AXP2101 PMIC over I2C**, not a GPIO gate / ADC divider — see `src/pmic.c`).
- The **XIAO ePaper Kit — EE02** shares the E1004's `spectra6_t133a01_dual`
  driver (same T133A01 panel), with only a different pin map.
- The **TRMNL 7.5" OG DIY Kit** shares the E1001's `mono_spi` driver (same
  800×480 mono panel), with its own pin map.

The three XIAO ESP32-S3 boards (PhotoPainter, EE02, TRMNL 7.5") are **native-USB**
(no CH340), so their console runs on USB-Serial-JTAG via
`sdkconfig.usbjtag.defaults` — which also frees UART0 (GPIO43/44) on the boards
that route those pins to the panel.

**Touch (reTerminal E1003 only).** The E1003's onboard **GT911** capacitive
digitiser can be enabled per-device from the server (Tesserae >= 0.140.0). It is
a deep-sleep wake source: a tap or swipe wakes the device, which reports the raw
stroke on the frame GET; the server classifies the gesture and repaints in the
same response (no on-device gesture logic). Battery cost: keeping touch armed
holds the GT911 scanning through deep sleep, drawing a few mA continuously, which
materially shortens battery life, so it is best used docked or on USB. Off by
default; a touch-less E1003 is unchanged, and the other seven boards build
byte-identical (all touch code is behind `#if BOARD_HAS_TOUCH`).

## Architecture

```
platformio.ini              one [env:...] per device (board macro -> driver)
boards/<board>.h            per-board pin map, geometry, palette, MCU tier,
                            device kind, and the selected PANEL_DRIVER_* macro
boards/board.h              dispatches on -DTESSERAE_BOARD_* -> the board header
src/panel/
  epd_panel.h               epd_driver_t vtable {port_init,init,clear,display,
                            show_color_bars,show_palette_sweep,sleep} + panel_info
  registry.c                build-time driver selection -> epd_active_driver()
  drivers/                  one self-contained driver per panel family, each
                            #if-guarded by its PANEL_DRIVER_* macro
src/
  main.c                    the wake cycle
  net_rest.c / rest_config  Tesserae REST client + NVS-backed config
  image_fetcher / _decoder  HTTP frame download + size-validated copy (no decode)
  provisioning.c            captive-portal setup (AP + DNS + scan + form)
  splash.c                  on-device procedural splashes (logo, portal QR,
                            connect-status messages), bpp-aware
  battery.c                 board-gated Li-Po telemetry (ADC or PMIC gauge)
  pmic.c                    AXP2101 PMIC over I2C (rails + battery), BOARD_HAS_PMIC
  sht4x.c                   E-Series environment telemetry, BOARD_HAS_SHT4X
  wifi_manager.c            WiFi STA/AP
```

Each concrete driver is a faithful port of a proven reference (Waveshare demo,
`bitbank2/bb_epaper`, or `bitbank2/FastEPD`), with byte-level
provenance in the source. Panel-specific quirks (dual-CS split, mirror, the
IT8951 load/waveform protocol) live in the driver; the rest of the firmware is
panel-agnostic and talks to the active panel only through the vtable.

## The wake cycle

```
boot
  -> no WiFi creds / no server URL?   -> captive portal -> reboot
  -> connect WiFi (STA)
  -> no device token?                 -> discover / register (onboard) -> sleep
  -> GET frame (If-None-Match)         304 -> skip paint
                                       204 -> nothing rendered yet
                                       200 -> download the panel-native .bin
  -> POST status (battery, rssi, optional temperature/humidity, next_poll_s)
  -> WiFi off
  -> paint the frame (radio off for the slow refresh)
  -> deep sleep for the server-driven interval
```

The wall clock is taken from each REST response's HTTP `Date` header (no SNTP),
and an unchanged frame (ETag/304) skips both the download and the paint.

### Onboarding feedback

The panel always tells the user where setup stands, so a headless device is
never a black box:

- **WiFi won't connect** or **server unreachable** (bad URL / server down) —
  the captive portal stays up and its subtitle says why (*"Wi-Fi didn't
  connect"*, *"Can't reach the server"*), so it can be fixed on the spot.
- **Reached the server, waiting for admin approval** — this is *not* a failure:
  the panel shows *"Almost done — approve this device in Tesserae"* and the
  device sleeps and retries (it does **not** reopen the portal).
- **Onboarded, no frame yet** — paints *"Connected! Waiting for your first
  frame"* so setup has clear closure; the frame lands on a later wake.

To avoid re-refreshing the slow panel on every retry, these status splashes
paint only on a cold / post-setup boot, not on timer wakes.

## Tesserae REST protocol

The device talks to `<server_url>/api/v1/device/`:

| Endpoint | Auth | Purpose |
| --- | --- | --- |
| `POST /discover` | none | Zero-touch onboarding. The admin approves the device in the Tesserae UI; the next discover returns the token (matched by MAC). |
| `POST /register` | `X-Pairing-Code` | Onboarding gated by a pairing code (idempotent). |
| `GET /<id>/frame` | `Bearer` + `If-None-Match` | Frame metadata + ETag; the `.bin` is fetched from the returned URL. |
| `POST /<id>/status` | `Bearer` | Telemetry (battery, rssi, ip, `fw_version`, optional environment); returns `next_poll_s` (drives the sleep) and config. |

The `/status` heartbeat JSON includes **`fw_version`**, the build's semantic
version with no leading `v` (for example `1.2.0`; untagged builds report
`0.0.<build>` or `0.0.0-dev`). The server compares it against the latest
available build to flag when an update can be flashed. Reporting is passive: the
firmware does no OTA.

Seeed reTerminal E1001-E1004 boards also report the onboard SHT4x reading as
**`temperature_c`** (degrees Celsius) and **`humidity_pct`** (relative humidity
percentage), plus `env_sensor: "sht4x"`. A failed sensor read omits these fields
without interrupting the heartbeat or frame cycle.

Each device reports a **kind** (`TESSERAE_DEVICE_KIND` in its board header) that
selects the server-side renderer. The server must produce the exact panel-native
format the firmware expects for that kind:

| Kind | Frame format | Size |
| --- | --- | --- |
| `waveshare_133e6`, `seeed_reterminal_e1004`, `seeed_ee02` | 4bpp packed Spectra-6 | 960000 B |
| `seeed_reterminal_e1002`, `waveshare_photopainter_73`, `seeed_ee04_73e6` | 4bpp packed Spectra-6 | 192000 B |
| `seeed_reterminal_e1001`, `xiao_epaper_75`, `seeed_ee04_75` | 1bpp packed mono (bit 1 = white) | 48000 B |
| `seeed_reterminal_e1003` | 4bpp packed grayscale (0=black…0xF=white) | 1314144 B |

The PhotoPainter reuses the E1002's 800×480 4bpp format exactly (render normally
— the **180° rotation is done on-device**, so do not pre-rotate on the server).

## Build

Requires [PlatformIO Core](https://platformio.org/install). Build one target:

```sh
pio run -e seeed-reterminal-e1002              # or any env from the table
```

The first build fetches the ESP-IDF toolchain (a few minutes); later builds take
~15–50 s. Output is under `.pio/build/<env>/`: `firmware.bin` (app, flashed at
`0x10000`) alongside `bootloader.bin` (`0x0`) and `partitions.bin` (`0x8000`).
Every push is built for all targets in CI (see
`.github/workflows/firmware.yml`), which stamps an auto-incrementing
`0.0.<build>` version (starting at `0.0.0`) into `FW_VERSION` and the uploaded
artifact / `.bin` names. Local builds fall back to `0.0.0-dev`. Tagged releases
(`.github/workflows/release.yml`) instead stamp the release tag's semantic
version with the leading `v` stripped, so tag `v1.2.0` reports `FW_VERSION`
`1.2.0` in the heartbeat while the artifact paths keep the `v` prefix.

## Flash

### Web flasher — easiest, no toolchain

The simplest way to flash a device is the browser flasher at
**[tesserae.ink/flash](https://tesserae.ink/flash)** — no PlatformIO, no esptool,
nothing to install. Plug the device in over USB, pick your board and version,
and click flash; it talks to the ESP32 directly over **WebSerial** (use Chrome
or Edge). Each release published here is built and uploaded automatically, so
the flasher always offers the latest firmware for every target.

One driver caveat: the **reTerminals** use a WCH CH340 bridge, so on macOS you
still need the CH34x driver below for the browser to see the port. The XIAO
boards (PhotoPainter, EE02, TRMNL 7.5") are native-USB and need no driver.

### From source (PlatformIO)

Prefer building yourself? Flash the env you [built](#build) over USB:

The **reTerminals** flash through an onboard **WCH CH340** USB-serial bridge
(not the ESP32-S3 native USB). On macOS install the WCH CH34x DriverKit driver
([WCHSoftGroup/ch34xser_macos](https://github.com/WCHSoftGroup/ch34xser_macos))
and enable it under *System Settings → General → Login Items & Extensions →
Driver Extensions*; the port then appears as `/dev/cu.wchusbserial*`.

The **XIAO ESP32-S3 boards** (PhotoPainter, EE02, TRMNL 7.5") have no CH340 — they
flash over the S3's **native USB-Serial-JTAG**, which enumerates as
`/dev/cu.usbmodem*` (no driver needed). These boards route the console to
USB-Serial-JTAG, so app logs *are* visible over that USB port (except the
PhotoPainter, whose console stays on UART0 — use the panel splashes there).

```sh
pio run -e <env> -t upload --upload-port /dev/cu.wchusbserial*   # reTerminals
pio run -e xiao-epaper-75 -t upload --upload-port /dev/cu.usbmodem*   # XIAO boards
```

If a native-USB board's port keeps flickering/disappearing (the app deep-sleeps,
which drops the USB), force ROM download mode: **hold BOOT, unplug, replug while
holding BOOT, release** — the port then stays steady for flashing.

Or with esptool directly (from the build dir):

```sh
esptool --chip esp32s3 --port <PORT> --baud 460800 write-flash --flash-size detect \
  0x0 bootloader.bin  0x8000 partitions.bin  0x10000 firmware.bin
```

- **Always `--flash-size detect`.** **Never erase-all (`-e`)** on a reflash — it
  wipes NVS (WiFi creds + device registration).
- **E1003 only:** its 32 MB flash trips esptool's stub loader (`attach_flash`
  fails); flash with `--no-stub` (the env pins this via `upload_flags`).
- App logs (and panic backtraces) come out the CH340/UART0 at 115200.

## Provisioning

A fresh device (no WiFi creds or no server URL) comes up as a captive-portal
setup AP:

1. It paints a setup splash (logo, the AP name, the portal URL, a join QR).
2. Join the **`Tesserae-Setup`** WiFi AP (scan the QR, or pick from the list;
   password `tesserae`).
3. Enter your WiFi and the Tesserae **server URL** (and an optional pairing code).
4. It reboots, onboards over REST, and appears in Tesserae → Settings → Devices
   for approval. Once approved it fetches and paints frames.

Development shortcut: copy `include/secrets.example.h` to `include/secrets.h`
(git-ignored) and set `WIFI_DEFAULT_*` / `REST_DEFAULT_SERVER_URL` to skip the
portal while iterating.

## Adding a board

1. `boards/<board>.h` — pin map, geometry, palette, `TESSERAE_DEVICE_MODEL`,
   `TESSERAE_DEVICE_KIND`, MCU tier, and the `PANEL_DRIVER_*` macro (reuse an
   existing driver, or point at a new one).
2. If it's a new panel family, add `src/panel/drivers/<driver>.c` (guarded by its
   `PANEL_DRIVER_*` macro, exporting an `epd_driver_t`) and an `#elif` in
   `boards/board.h` and `src/panel/registry.c`.
3. Add an `[env:...]` (+ `-selftest`) in `platformio.ini` and to the CI matrix.
4. Server side: register the device kind + a renderer that emits the panel-native
   frame format.

## Credits

The panel hardware facts, pin maps, and init/refresh sequences build on the
work of several open-source projects, with thanks:

- **[usetrmnl/trmnl-firmware](https://github.com/usetrmnl/trmnl-firmware)** —
  reference firmware for the Seeed reTerminal e-paper devices.
- **[bitbank2/bb_epaper](https://github.com/bitbank2/bb_epaper)** and
  **[bitbank2/FastEPD](https://github.com/bitbank2/FastEPD)** — the underlying
  panel/controller drivers (Spectra-6, UC8179 mono, IT8951 grayscale).
- the **Waveshare 13.3E6** ESP-IDF demo and the
  **[Pimoroni Inky](https://github.com/pimoroni/inky)** drivers.
- **[waveshareteam/ESP32-S3-PhotoPainter](https://github.com/waveshareteam/ESP32-S3-PhotoPainter)**
  and **[aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe)**
  for the PhotoPainter's ED2208-GCA panel and AXP2101 PMIC bring-up.

Bundled third-party code: the public-domain font8x8, and the MIT
[qrcodegen](https://github.com/nayuki/QR-Code-generator) (`src/vendor/`).

## License

AGPL-3.0-or-later (see [LICENSE](LICENSE)), matching the sibling
`tesserae-device-*` repositories.
