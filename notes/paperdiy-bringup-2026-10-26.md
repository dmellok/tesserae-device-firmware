# M5Stack PaperDIY bring-up, 2026-10-26

Unit: PaperDIY (Complete Kit K159). Console on the S3's own USB-Serial-JTAG
(no CH340). ESP-IDF 5.5.2, chip revision v0.2. Board header
`boards/m5stack_paperdiy.h`. The `-selftest` build ran first; the production
build followed against a local Tesserae server and is registered and painting
(see "The run that followed").
## Download mode

Long-pressing the power button (~2 s; the duration was not measured) puts the
unit in ROM download mode. Confirmed positively: with the app running,
`esptool --before no-reset chip-id` fails ("No serial data received"); after the
long-press it connects and detects the ESP32-S3.

The red LED blinks during the press. In ROM download mode no application runs,
so the blink is the PM1's. A single click afterwards boots the app.

Ordinary flashing does not need the button: `-t upload` auto-resets over
USB-Serial-JTAG. That was true with this firmware already on the unit, so it
says nothing about a from-factory unit.

## Red LED

Boot line `m5pm1: PWR_CFG 0x17` decodes (`src/m5pm1.c`) as CHG=1 DCDC=1 LDO=1
BOOST=0 LED_EN=1. `pm1_rmw()` refuses a `PWR_CFG` write unless DCDC and LDO both
read as on, which they do here, so LED writes are accepted. No `PMIC write
failed` line appeared on any boot.

The diode is lit steady on a cold boot: `led_set(false)` only runs on a
deep-sleep wake, so a cold boot leaves it on until a frame paints. It is the
only indicator on a board with no BOOT button.

## Power button

The button belongs to the PM1 and is wired to the chip's reset, so the firmware
never sees it as a wake source.

- Single click while running: the chip hard-resets and the boot restarts from 0.
- Double click: powers off. Watched at the USB node, which went absent at 13.0 s
  and returned at 18.7 s when clicked back on.
- Single click out of download mode: boots the app.

A press is a hard reset, so `first_boot` is always true and the board takes the
cold-boot path. There is no power-button wake case to handle.

Open: the `reset_reason` for a button press could not be read (a reset over
USB-Serial-JTAG costs ~1.6 s of re-enumeration and the line prints at ~0.6 s),
and `diag_reset_t` has no value for it, so it latches as `DIAG_RESET_NONE`.
Also untested: a double click during a paint.

## Panel rail on the PMIC alone

The `EPD_PIN_PWR 45` fallback is not needed. The rail comes up through
`EPD_PWR_M5PM1_PIN` (PY_G2) and nothing else.

The `-selftest` painted the full sequence: scrambled sheet at 1550 ms, ordered
sheet done at ~47.5 s, then `done; halting`. On the glass: 16 bars, evenly
spaced, level 0 black at one edge through level 15 white at the other.

Not judged: the sheet's two halves carry the shipped matrix and
`EPD_PAR_GRAY_MATRIX_B`; the comparison between them was not made. See
`notes/papers3-gray-matrix-tuning.md`.

## Also verified

- 8 MB octal PSRAM, 16 MB flash, `nvs` at `0x9000/0x6000` and `phy_init` at
  `0xf000/0x1000`: the factory layout matches `partitions_ota.csv`, so no NVS
  migration is needed.
- `m5pm1: VBAT` reads ~4.13 V on USB.
- The serial setup console answers `{"tesserae":"setup","cmd":"get"}` with
  `"kind":"m5stack_paperdiy"`.

## End to end against a local server

- The production build painted the portal splash (`epd_par: refresh done`).
  Driver side only; legibility on the glass was not checked.
- Wi-Fi and `server_url` were set over the serial setup protocol
  (`docs/serial-setup-protocol.md`), the board restarted, joined and began
  `rest discover`.
- The device registers with kind `m5stack_paperdiy`. Status arrives with
  battery, RSSI and OTA fields.
- The frame endpoint first answered 204 No Content because a fresh instance had
  no page and the device was bound to nothing: the missing assignment, not a
  rendering fault.- A clock dashboard arrived upright on the glass in landscape, held the way the
  board is meant to be used. The board does not define
  `BOARD_FRAMEBUFFER_ROTATION`, so it registers as 960x540 landscape, which is
  correct for this orientation.

## Battery on a 140 mAh cell

Production firmware, USB unplugged, deep-sleeping on the default 60 s interval,
VBAT as the PMIC reports it at boot each wake.

| Sample | VBAT | % |
| --- | --- | --- |
| on USB, idle | 4112 mV | 87 |
| 1st wake on battery | 4074 mV | 82 |
| 2nd | 4076 mV | 82 |
| 3rd | 4096 mV | 85 |
| 4th | 4100 mV | 86 |
| 5th | 4072 mV | 82 |
| 6th | 4100 mV | 86 |

Wakes are clean and periodic (`reset_reason=8`, `wakeup_cause=4`, `first_boot=0`
then `deep sleep for 60 s`), about 63 s apart, with no brownout, panic or
watchdog. The board survives its own battery duty cycle at a full cell.

Not measured:

- No paint happened on battery: the frame never changed, so every wake answered
  `304` and skipped the paint.
- VBAT during a paint is not instrumented on any board; `battery_read_mv()` is
  not called inside `frame_paint()`.
- A 2 h run (537 wakes) is too noisy to give a discharge curve: per-wake stdev
  is ~6.6 mV against a drift of a few mV per hour.
- The OTA gate (`TESSERAE_OTA_MIN_BATTERY_MV` 3600, ~22 % on this board's curve)
  was not stressed. The cell sat at 4.07-4.12 V throughout.

## The run that followed

On the same unit, production env flashed, server on the LAN:

- Registration as kind `m5stack_paperdiy`, reporting `panel 960x540`.
- The frame endpoint served a signed artifact of exactly 259200 B once a page
  existed; the panel painted it upright (landscape, no rotation).
- `/status` carries battery, RSSI, IP and firmware.
- Deep-sleep wakes (`reset_reason=8 wakeup_cause=4`) landed on the server's
  interval, 895-907 s measured across three cycles.

### Still not run on hardware

- (Resolved 2026-10-28, reported on PR #55: both keys verified. KEY1 = GPIO4 =
  the left key, M5's "A"; KEY2 = GPIO3 = the right key, "B"; both wake from
  ext1 and report themselves.)
- OTA on this board.
- Battery sag during a paint: `battery_read_mv()` does not run inside
  `frame_paint()`, so there is no instrument. The only battery data is per-wake
  voltage (4058-4140 mV, wobbling ~30 mV on the flat part of the curve).
- A double click during a paint: whether the panel is left half-painted.
- The captive portal's form: setup used the serial protocol, so the hotspot page
  was never submitted from a browser (the portal splash did paint).
- A ~4-minute burst of extra wakes, 33-75 s apart, seen while a view was being
  edited. Unexplained: a button press and the retry path are both candidates.

## Reproduction

```bash
pio run -e m5stack-paperdiy-selftest
pio run -e m5stack-paperdiy-selftest -t upload --upload-port <port>
```

Reading the boot log needs a reset the USB device survives, which it does not:
toggle DTR/RTS in esptool's `USBJTAGSerialReset` order, then reopen the port in
a retry loop. Lines before ~1.6 s are lost.
