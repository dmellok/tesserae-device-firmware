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
| Seeed reTerminal **E1001** (4-gray build) | 4-level grayscale | UC8179 (register LUTs) | 800×480, 2bpp | `mono_spi` (`EPD_GRAY4`) | `seeed-reterminal-e1001-gray` |
| [Seeed reTerminal **E1002**](https://www.seeedstudio.com/reTerminal-E1002-p-6533.html) | Spectra-6, single | UC81xx | 800×480, 4bpp | `spectra6_spi_single` | `seeed-reterminal-e1002` |
| [Seeed reTerminal **E1003**](https://www.seeedstudio.com/reTerminal-E1003-p-6731.html) | Grayscale (10.3") | IT8951 | 1872×1404, 4bpp gray | `it8951_gray` | `seeed-reterminal-e1003` |
| [Seeed reTerminal **E1004**](https://www.seeedstudio.com/reTerminal-E1004-p-6692.html) | Spectra-6, dual-chip | T133A01 | 1200×1600, 4bpp | `spectra6_t133a01_dual` | `seeed-reterminal-e1004` |
| [Seeed **XIAO ePaper Kit — EE02**](https://www.seeedstudio.com/XIAO-ePaper-DIY-Kit-EE02-for-13-3-Spectratm-6-E-Ink.html) | Spectra-6, dual-chip | T133A01 | 1200×1600, 4bpp | `spectra6_t133a01_dual` | `seeed-ee02` |
| [**TRMNL 7.5" OG DIY Kit**](https://www.seeedstudio.com/TRMNL-7-5-Inch-OG-DIY-Kit-p-6481.html) | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `xiao-epaper-75` |
| XIAO driver board + 7.5" **B/W/Red** panel (DKE DEPG0750RW / GDEW075Z08 class) | Tri-color BWR | UC8179 | 800×480, 2bpp | `mono_spi` (`EPD_BWR`) | `xiao-epaper-75-bwr` |
| [Seeed **XIAO ePaper Display Board — EE04**](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-EE04-p-6560.html) + 7.5" mono (24-pin) | Mono B/W | UC8179 | 800×480, 1bpp | `mono_spi` | `seeed-ee04-75` |
| [Seeed **XIAO ePaper Display Board — EE04**](https://www.seeedstudio.com/XIAO-ePaper-Display-Board-EE04-p-6560.html) + 7.3" Spectra-6 (50-pin) | Spectra-6, single | UC81xx | 800×480, 4bpp | `spectra6_spi_single` | `seeed-ee04-73e6` |
| [Waveshare **ESP32-S3-ePaper-13.3E6**](https://www.waveshare.com/esp32-s3-epaper-13.3e6.htm) | Spectra-6, dual-controller | UC81xx ×2 | 1200×1600, 4bpp | `spectra6_spi_dual` | `waveshare-133e6` |
| [Waveshare **PhotoPainter 7.3"**](https://www.waveshare.com/esp32-s3-photopainter.htm) | Spectra-6, single | ED2208-GCA | 800×480, 4bpp | `spectra6_spi_single` | `waveshare-photopainter-73` |
| [**M5Stack PaperS3**](https://docs.m5stack.com/en/core/PaperS3) | Grayscale (4.7") | none (raw parallel glass) | 960×540, 4bpp gray | `parallel_epd_gray` | `m5stack-papers3` |
| **Xteink X4** | Mono B/W (4.26") | SSD1677 | 800×480, 1bpp | `ssd1677_gray` (`EPD_MONO`) | `xteink-x4` |

The four reTerminals, the PhotoPainter, the EE02, and the TRMNL 7.5" kit have been
verified end-to-end on real hardware; the Waveshare 13.3E6 is the seed target and
builds green. The EE04 pair builds green but is **not yet hardware-verified**
(pin map taken from Seeed_GFX; the EE04 takes one panel on either its 24-pin or
50-pin FPC — flash the env matching the attached panel and set the jumper caps
accordingly). The **Xteink X4** is verified on hardware; note that later X4
production runs ship a UC8179 or UC8279 in place of the SSD1677 on the same
board and glass, and `xteink-x4` is the SSD1677 build.

Each board also has a `…-selftest` env that paints a driver-only
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
- The **Xteink X4** shares the Sticky's `ssd1677_gray` driver — same controller,
  800×480, active-HIGH BUSY. It is not mounted rotated (the driver derives that
  from the geometry), runs the driver's 1bpp `EPD_MONO` path, and needs
  `EPD_MIRROR_Y`. It is also the only board here **flashed app-only, keeping its
  factory bootloader** — see "Flashing the Xteink X4" below.

The exception is the **M5Stack PaperS3**, which needed a driver family of its
own because it is the first panel here with no controller chip. `parallel_epd_gray`
shifts 2-bit drive codes into the panel's source driver over the ESP32-S3's
LCD/i80 bus and walks the gate driver by hand on SPV/CKV; the 16 grey levels come
from a per-level pass matrix in the board header rather than a controller's
waveform flash, so retuning the greys is a table edit. Every other epdiy-class
board (Inkplate, LilyGo T5, epdiy V7) has this same shape, so the next one is a
pin map plus a matrix. Sequences ported from bitbank2's FastEPD. **Not yet
confirmed on physical hardware**: flash `m5stack-papers3-selftest` and judge
the 16 grey bands first.

The three XIAO ESP32-S3 boards (PhotoPainter, EE02, TRMNL 7.5") are **native-USB**
(no CH340), so their console runs on USB-Serial-JTAG via
`sdkconfig.usbjtag.defaults` — which also frees UART0 (GPIO43/44) on the boards
that route those pins to the panel. The **PaperS3** takes the same base for the
same reason: it is native-USB too, though it is not a XIAO board.

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
available build to decide whether an update can be offered.

**Wi-Fi OTA**: every board env builds with `TESSERAE_OTA_CAPABILITY_ENABLED`
on an A/B slot layout (`partitions_ota.csv`; the E1004 has its own identical
table). The heartbeat advertises `{"ota": {"schema": 1}}`; the server may
answer with a signed descriptor, which the firmware verifies against its
baked-in Ed25519 public key before streaming the image into the inactive slot,
rebooting, and self-confirming (ESP-IDF rollback reverts a bad image). Devices
flashed before their board's OTA-enabled release need a one-time USB or
webflasher migration to the A/B layout; NVS (credentials + registration)
survives when flashing via the catalog's offset-addressed `parts`.

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
| `seeed_reterminal_e1001`, `xiao_epaper_75`, `seeed_ee04_75`, `xteink_x4` | 1bpp packed mono (bit 1 = white) | 48000 B |
| `seeed_reterminal_e1001_gray` | 2bpp packed 4-gray (4 px/byte, MSB-first, 0b00=black..0b11=white) | 96000 B |
| `xiao_epaper_75_bwr` | 2bpp packed BWR (4 px/byte, MSB-first, 0=black 1=white 2=red, 3 reserved) | 96000 B |
| `seeed_reterminal_e1003` | 4bpp packed grayscale (0=black…0xF=white) | 1314144 B |
| `m5stack_papers3` | 4bpp packed grayscale (0=black…0xF=white) | 259200 B |

The PhotoPainter reuses the E1002's 800×480 4bpp format exactly (render normally
— the **180° rotation is done on-device**, so do not pre-rotate on the server).

### Deck frame cache (SD card)

Boards with a microSD slot (both Waveshares and all four reTerminals) cache
pre-rendered "deck" pages on the card so a button/touch navigation becomes
wake → SD read → paint (1–2 s, radio off) instead of wake → Wi-Fi → fetch
(4–8 s). Entirely runtime-gated: card present and mountable → the device
advertises `"deck_cache": {"schema": 1, "capacity_bytes": …}` in register and
status bodies; no card → wire bodies and behaviour are identical to before.
The server binds a deck via `GET /api/v1/device/<id>/deck` (manifest of pages,
16-hex sha256 digests, byte sizes, TTLs, and button/zone links) and announces
version changes in the status response's `"deck": {"version"}`; the device
syncs at the tail of a scheduled wake (fetch missing digests, delete orphans)
and reports SD-served pages via `deck_page_id`/`deck_version`. Locally served
presses are **not** sent as button/touch actions. Every cached frame is
verified (exact length + digest, mbedTLS SHA-256) before painting; any
mount/read/parse failure falls back to the network path. Card layout:
`/tesserae/decks/<deck_id>/manifest.json` + `<digest>.bin`. Bring-up: the
`…-sdtest` env runs a mount + write/read/verify round trip over serial.

### Local overlay rendering (hybrid render mode)

Touch boards with a partial-refresh panel (currently the E1003 / IT8951)
advertise `"overlay": {"schema": 1}` and can echo taps and update small value
slots locally in under a second, without a server round trip per interaction.
The server's full frame stays the base layer and source of truth: after each
full paint the firmware fetches `GET /frame/overlay/<digest>` (targets =
tap-echo rects, slots = value text fields, atlases = pre-rendered glyph
strips) and `GET /frame/data?digest=…` for pre-formatted value strings
(polled only inside the touch-linger awake window; `overlay_values` on
/status is treated identically, newest `seq` wins). Tap echoes invert their
rect with a fast DU partial refresh and never delay the normal stroke
dispatch; text is blit-only from the atlas. After 8 partial refreshes (or on
any new full frame) a GC16 full-quality pass clears ghosting. Specs, atlases
and rect patches are cached on SD keyed by frame digest so a tap that wakes
the device echoes offline. A server without overlay support (404) leaves the
feature fully dormant. Bring-up: the `…-overlaytest` env runs a synthetic
spec (invert target + digits atlas) with timings on serial.

### Device-owned touch (touch v3)

The newest interaction model inverts the old one: instead of the server
extracting hotspots from HTML, hit-testing coordinates, and shipping frame
patches back, **the device draws the controls and owns the interaction.** The
server renders the dashboard image with the control rects left *blank* and
serves a small declarative spec; the firmware fills those rects with four
primitives — `button`, `switch`, `slider`, `stepper` — hit-tests locally,
gives instant partial-refresh feedback, and reports a *semantic* event
(which control, what interaction, what value). Action payloads never reach
the device; it learns only a tier and a type.

Boards with touch + partial refresh advertise `"touch": {"v": 3,
"primitives": [...]}` alongside `panel.depth_bpp` / `panel.grayscale`,
`partial_refresh` and `can_stay_awake`. That last one is a hardware fact — the
device *can* run without deep sleep — and is what lets the server offer it
interactive controls. Whether it actually stays awake is **server config**
(`config.always_on` on the /status response, on the same channel as
`sleep_interval_s`), adopted on every poll, so flipping it takes effect on the
next poll with no reboot: true holds WiFi, the digitizer and the SSE state
stream; false runs the normal sleep cycle, draws the primitives statically from
the spec's values, and each interaction wakes → reports → sleeps.

Per frame the firmware pulls `GET /frame/spec?layout=<held>`. That param is
*advisory* — the server answers for the device's current frame regardless — so
the **returned** `layout_digest` is what detects a layout change (it is stable
across data-only redraws: a clock tick does not invalidate it), and a 404/204 is
what means "no touch on this frame". A matching digest reuses the parsed spec
and its loaded atlases, adopting only the freshly seeded `state`/`value`, which
are data and can move under an unchanged layout. Uncached
`GET /atlas/<digest>` strips are fetched, the primitives are composed into the
frame *before* the paint so one GC16 shows image and controls together, and
interactions go to `POST /interact`.

`/interact` replies carry only `{outcome, primitive_id}` and fire the side
effect with no server re-render — the device owns feedback. The outcome is
logged, never branched on, since the pixels already moved. Reconciliation comes
instead from the **values stream**: `GET /frame/stream` (SSE) emits
`{"seq":…,"values":{"ha:light.desk":"on"}}` keyed by `value_key`, and the
firmware maps those keys onto its own primitives — that mapping is the device's
job, since the server doesn't know which primitives exist. Newest `seq` wins,
several primitives may share a key, and the same envelope arrives on `/status`
so a battery device without SSE still corrects on each wake.

The `touch_v3` feature is behind a server experiment that is **off by default**,
and the off state is an empty `primitives` array — valid, not an error. v3 then
holds nothing and declines each stroke so the existing v2/v1 paths handle it
exactly as they do today; swallowing it would disable working touch on every
device with the flag off.

Geometry comes from the contract's shared `primitives.json`, which the server's
canvas preview draws from too — that shared source of truth is what makes the
device output match the preview, and geometry drift is the main fidelity risk.
Two conventions meet in the renderer and are worth knowing before editing it.
Spec rects are **final device-framebuffer coordinates**: the firmware draws at
each rect exactly as given and only maps GT911 points through the board's touch
calibration — there is deliberately no canvas→panel scaling or rotation on the
device, because that transform is the server's job. (It is still a pending
server task, so until it lands rects may land off-target on the glass; that is
expected, and the fix belongs on the server, not here.) And the contract's ink
scale is **inverted** relative to the panel's (`0 = paper … 15 = ink` versus the
framebuffer's `0x0 = black … 0xF = white`), so every draw op and atlas blit
inverts on the way out.

The renderer is panel-agnostic — every entry point takes
`(fb, width, height, bpp)` and no board symbols — so a new touch-capable board
needs no changes to it, only a touch-controller driver and the two board gates
(`BOARD_HAS_TOUCH` + `BOARD_OVERLAY_PARTIAL`; instant local feedback is
impossible without partial refresh, so colour/Spectra panels are out regardless
of digitizer). At **1bpp** the four-level palette folds deliberately: only
`paper` stays white, so a stepper's `soft` dividers and a switch's `mid` on-track
stay visible, and the switch thumb flips to paper over a filled track instead of
disappearing into it. Glyph coverage is continuous tone and thresholds at the
midpoint rather than inking every level, so mono text keeps its intended weight.
`panel.grayscale` comes from the active driver's `epd_panel_info_t`, so a new
panel family declares it once and cannot silently lose duotone.

Atlases are **text only** — a 4bpp-gray strip of advance-width cells (`adv ==
w`), blitted per character with the cursor advancing by `adv`, vertically centred
on `ascent`/`descent`. Two roles: `l20` for labels (20 px/400) and `v28` for
values (28 px/700). Value boxes are sized from the widest digit advance plus the
suffix rather than the current string, so a slider's readout doesn't twitch
between `9%` and `100%`.

Icons render from a **bundled Phosphor weight** (`icon.name` → codepoint), not
from the atlas. The firmware ships Phosphor **2.1.2 bold**
([src/fonts/Phosphor-Bold.ttf](src/fonts/), MIT, 484 KB) plus `stb_truetype`,
and resolves names through [include/phosphor_codepoints.h](include/) — a
1530-entry table generated from that release's stylesheet by
[tools/gen_phosphor_codepoints.py](tools/gen_phosphor_codepoints.py), which
*requires* `--version` so an unpinned map can't ship by accident.

That pin is verified rather than assumed: the server's own vendored
`Phosphor-Bold.woff2` is **byte-identical** to official 2.1.0/2.1.1/2.1.2 (same
SHA-256), and the codepoint maps match with zero conflicts — the bold font and
its codepoints didn't change across the 2.1.x line, so both sides resolve every
name to the same glyph. [tools/test_icons.sh](tools/test_icons.sh) enforces it
in CI by checking all 1530 mapped names against the bundled font; a map
regenerated from a different release fails there instead of silently drawing
wrong pictures on a panel.

Glyphs scale by **em box** (`stbtt_ScaleForMappingEmToPixels`), matching the CSS
`font-size` the server preview renders at — scaling by ascent/descent would draw
a visibly different size. Phosphor can rasterize a pixel over the em box, so the
bitmap is centred in the reserved `px` cell and clipped; the contract guarantees
glyph-identity, not pixel-identity. The font costs ~484 KB of flash and is
enabled per-env (`-DT3_HAVE_ICON_FONT` plus the two embed declarations), so
boards that don't render icons carry none of it; drop the flag and buttons lay
out label-only with the `icons` capability unadvertised.

A server that sends no `layout_digest` leaves v3 fully dormant and the
protocol-v2 then v1 touch paths run unchanged. Pure logic (spec parse, hit
test, snap/axis math, geometry, draw ops) lives in `touch3.c` and is host-tested
by `tools/test_touch3.sh`; device orchestration is `touch3_run.c`. Bring-up:
the `…-touch3test` env renders all four primitives from a synthetic spec and
exercises every interaction with per-op timings on serial, then runs a 30 s
live-tap orientation check — no networking, so it validates the panel and
geometry before the server endpoints exist.

### Cloud relay (remote panels)

A panel can run somewhere the Tesserae server isn't reachable from — a different
house, a phone hotspot, behind CGNAT — without exposing the home instance to the
internet. Both ends talk **outbound** to a small Cloudflare Worker that acts as a
per-device mailbox: home seals each rendered frame and `PUT`s it, the panel polls
and decrypts it.

The relay is **zero-knowledge**. At pairing the panel and the home instance
exchange X25519 *public* keys through it and each derive the same AES-256-GCM
frame key locally; the key is never transmitted. The relay only ever holds
ciphertext, two public keys and a token hash, so it can validate a poll but
never read a dashboard.

Setup is one extra card in the captive portal: a relay URL (defaults to
`https://relay.tesserae.ink`) and the single-use pairing code from
`Settings → Cloud relay → Add a remote panel`. A relay panel needs **no**
server URL — the portal accepts either. On the next wakes the device posts its
public key, polls until the home instance completes the exchange, derives the
frame key, and stores it in NVS; the private scalar is wiped once the key
exists, since only the key is needed thereafter.

In steady state each wake is one conditional `GET .../frame` with
`If-None-Match` (304 keeps the current image, 204 means nothing published yet)
plus a `POST .../status` carrying the same telemetry body a local device sends,
so battery / signal / firmware / last-seen still populate the Devices UI. Frames
decrypt **in place** — a 1.3 MB frame would otherwise need a second buffer — and
a failed authentication tag is never painted: it means the mailbox doesn't match
this panel's key.

The wire contract is `docs/relay/contract.md` in the Tesserae repo, and it is
authoritative for all three sides. Its golden vectors are checked in CI by
[tools/test_relay_crypto.sh](tools/test_relay_crypto.sh), which compiles the real
device crypto (Monocypher X25519 + ESP-IDF mbedTLS HKDF/AES-GCM) rather than a
host stand-in — a derivation that is merely plausible produces a wrong key, and
the only symptom in the field is that frames silently never appear.

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

### Flashing the Xteink X4

The X4 keeps its **factory bootloader** and takes the app alone, at `0x10000`:

```sh
pio run -e xteink-x4 -t upload          # plug USB-C, tap power, run this
```

`tools/upload_app_only.py` drops the bootloader and partition-table images so
the short command is the safe one. This is a sealed reader with **no exposed
BOOT strap**, so a unit that will not enumerate needs the case opened; leaving
the factory bootloader in place keeps a bad app recoverable, keeps the vendor
SD-card update path (the only route into a USB-locked unit), and lets the
original firmware go back.

The web flasher offers the X4 under the same rule: its catalog entry carries
only the otadata + app parts at their stock offsets (never a bootloader, never
a partition table), and the factory-reset mass erase is withdrawn for it, since
a full erase would take the factory bootloader with it.

**Tap the power button first.** In deep sleep the C3 powers down its
USB-Serial-JTAG, so the port does not exist and esptool has nothing to reset.
One tap boots it; the firmware then sees a USB data host and loops instead of
sleeping, so the port stays put. The same cable carries the log (`-t monitor`).

Because the bootloader is inherited, so is the partition table:
`partitions_xteink_x4.csv` transcribes the stock layout. On a unit of unknown
provenance, check it first:

```sh
esptool --chip esp32c3 --port <PORT> read-flash 0x8000 0xc00 stock.bin
```

Use `-e xteink-x4-full` to deliberately write a bootloader and table.

## Provisioning

A fresh device (no WiFi creds or no server URL) comes up as a captive-portal
setup AP:

1. It paints a setup splash (logo, the AP name, **the AP password**, the portal
   URL, a join QR).
2. Join the **`Tesserae-Setup`** WiFi AP (scan the QR, or pick from the list;
   password `tesserae`, also printed on the splash and in the serial log).
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
