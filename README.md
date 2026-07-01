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
| Waveshare 13.3" E6 | Spectra-6, dual-controller | UC81xx ×2 | 1200×1600, 4bpp | `spectra6_spi_dual` | `waveshare-133e6` |
| Seeed reTerminal **E1004** | Spectra-6, dual-chip | T133A01 | 1200×1600, 4bpp | `spectra6_t133a01_dual` | `seeed-reterminal-e1004` |
| Seeed reTerminal **E1002** | Spectra-6, single | UC81xx | 800×480, 4bpp | `spectra6_spi_single` | `seeed-reterminal-e1002` |
| Seeed reTerminal **E1001** | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `seeed-reterminal-e1001` |
| Seeed reTerminal **E1003** | Grayscale (10.3") | IT8951 | 1872×1404, 4bpp gray | `it8951_gray` | `seeed-reterminal-e1003` |

The four reTerminals have been verified end-to-end on real hardware; the
Waveshare 13.3E6 is the seed target and builds green. Each board also has a
`…-selftest` env that paints a driver-only test pattern (colour bars / gray
ramp) with no networking — flash that first when bringing up a new unit.

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
  splash.c                  on-device procedural splash (logo + QR), bpp-aware
  battery.c                 board-gated Li-Po telemetry
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
  -> POST status (battery, rssi, next_poll_s)
  -> WiFi off
  -> paint the frame (radio off for the slow refresh)
  -> deep sleep for the server-driven interval
```

The wall clock is taken from each REST response's HTTP `Date` header (no SNTP),
and an unchanged frame (ETag/304) skips both the download and the paint.

## Tesserae REST protocol

The device talks to `<server_url>/api/v1/device/`:

| Endpoint | Auth | Purpose |
| --- | --- | --- |
| `POST /discover` | none | Zero-touch onboarding. The admin approves the device in the Tesserae UI; the next discover returns the token (matched by MAC). |
| `POST /register` | `X-Pairing-Code` | Onboarding gated by a pairing code (idempotent). |
| `GET /<id>/frame` | `Bearer` + `If-None-Match` | Frame metadata + ETag; the `.bin` is fetched from the returned URL. |
| `POST /<id>/status` | `Bearer` | Telemetry (battery, rssi, ip); returns `next_poll_s` (drives the sleep) and config. |

Each device reports a **kind** (`TESSERAE_DEVICE_KIND` in its board header) that
selects the server-side renderer. The server must produce the exact panel-native
format the firmware expects for that kind:

| Kind | Frame format | Size |
| --- | --- | --- |
| `waveshare_133e6`, `seeed_reterminal_e1004` | 4bpp packed Spectra-6 | 960000 B |
| `seeed_reterminal_e1002` | 4bpp packed Spectra-6 | 192000 B |
| `seeed_reterminal_e1001` | 1bpp packed mono (bit 1 = white) | 48000 B |
| `seeed_reterminal_e1003` | 4bpp packed grayscale (0=black…0xF=white) | 1314144 B |

## Build

Requires [PlatformIO Core](https://platformio.org/install). Build one target:

```sh
pio run -e seeed-reterminal-e1002              # or any env from the table
```

The first build fetches the ESP-IDF toolchain (a few minutes); later builds take
~15–50 s. Output is under `.pio/build/<env>/` (`firmware.bin`, plus a combined
`firmware.factory.bin`). Every push is built for all targets in CI (see
`.github/workflows/firmware.yml`), which stamps an auto-incrementing
`0.0.<build>` version (starting at `0.0.0`) into `FW_VERSION` and the uploaded
artifact / `.bin` names. Local builds fall back to `0.0.0-dev`.

## Flash

Devices flash through an onboard **WCH CH340** USB-serial bridge (not the ESP32-S3
native USB). On macOS install the WCH CH34x DriverKit driver
([WCHSoftGroup/ch34xser_macos](https://github.com/WCHSoftGroup/ch34xser_macos))
and enable it under *System Settings → General → Login Items & Extensions →
Driver Extensions*; the port then appears as `/dev/cu.wchusbserial*`.

```sh
pio run -e <env> -t upload --upload-port /dev/cu.wchusbserial*
```

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

Bundled third-party code: the public-domain font8x8, and the MIT
[qrcodegen](https://github.com/nayuki/QR-Code-generator) (`src/vendor/`).

## License

AGPL-3.0-or-later (see [LICENSE](LICENSE)), matching the sibling
`tesserae-device-*` repositories.
