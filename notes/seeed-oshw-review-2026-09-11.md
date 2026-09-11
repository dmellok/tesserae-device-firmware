# Seeed OSHW reTerminal firmware hub: what to bring across

Reviewed 2026-09-11 against https://github.com/Seeed-Projects/OSHW-reTerminal-Series-E-D
(commit of 2026-08-31). The hub carries Seeed's Arduino examples, the official
TRMNL builds (Arduino + an ESP-IDF E1004 tree), SenseCraft HMI, EEZ Studio and
LVGL panels, plus their browser flasher. Paths below: `oshw/` = that repo,
`fw/` = this one. Line numbers are from the review date.

## Tier 1: cheap, high confidence

1. **Light-sleep the SoC during panel BUSY waits.** TRMNL wraps every BUSY
   wait in `esp_light_sleep_start()` with a timer wake
   (`oshw/examples/official/TRMNL/src/display.cpp:462-475`), on for
   E1001/E1002/E1003. Ours spin on `vTaskDelay` (`spectra6_spi_single.c:51-57`,
   `spectra6_t133a01_dual.c:146-147`, `it8951_gray.c:144-145`,
   `mono_spi.c:134-135`). A Spectra paint is 25-30 s at active current, which
   is more charge per cycle than a day of deep sleep. Wi-Fi is already stopped
   before the paint in the normal path (`main.c:2811-2835`); deck-sync and
   linger wakes keep it up and need modem sleep or a skip. Arm a GPIO level
   wake on `EPD_PIN_BUSY` (polarity differs: IT8951 HRDY is high = ready) plus
   a 1 s timer backstop. Bench: paint-phase current on E1002 and E1003.
2. **Per-board battery SOC curves.** SenseCraft ships 101-point tables per
   model (`oshw/.../SenseCraft_HMI/src/boards/reterminal_e1001/config.h:6-108`,
   same for e1003, e1004), all topping out at ~4.11 V (the charger's real
   termination). Our two-segment line (`fw/src/battery.c:37-43`) reports 30 %
   at 3.70 V where Seeed's table says ~60 %, and never reaches 100 %. Add a
   `BOARD_BATTERY_CURVE` hook and drop the three tables into the headers.
   Side effect: `AWAKE_BATTERY_MIN_PCT 15` moves from ~3.50 V to ~3.43 V.
3. **Use the PCF8563 RTC** (0x51 on I2C0 GPIO19/20, CR1220 backup) on all four
   reTerminals. Seeed: `oshw/examples/base/RTC_PCF8563/RTC_PCF8563.ino`
   (init 191-199, VL flag 202-207, `settimeofday` 270-282); SenseCraft writes
   it back after every time sync. We never touch it; wall clock comes only
   from the HTTP Date header. Do: seed `settimeofday` at cold boot when VL is
   clear and year >= 2025; write back when the Date discipline moves the
   clock > 2 s; write REG 0x0D = 0 once (CLKOUT off, it powers up enabled at
   32.768 kHz). Whether INT reaches an RTC-capable GPIO (crystal-accurate
   long sleeps) is not in the repo; needs the schematic.
4. **Cache the pre-radio battery read for the whole wake.** TRMNL reads once
   before Wi-Fi (`TRMNL/src/bl.cpp:1164`). We read at `main.c:1125`, discard
   it, then read again under TX load in `net_rest.c:1010-1012`, plus
   `ota_install.c:40` and `ble_setup.c:287`. The BQ27220 backend already
   caches (`bq27220.c:46, 80-83`).
5. **Bound the E1004 BUSY waits.** `spectra6_t133a01_dual.c:142-153` warns at
   60 s then waits forever, and `update_phase` (158-174) polls before BUSY
   asserts. Port mono_spi's assert-then-stable bounded helper
   (`mono_spi.c:104-169`). Seeed bounds PON 5 s / DRF 60 s / POF 5 s
   (`GxEPD2_T133A01_1200x1600.cpp:120-155, 586-596`).
6. **`CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y`** (TRMNL
   `sdkconfig.TRMNL_X_E1003:480`). We re-hash the app on every timed wake.
   Put it in `sdkconfig.ota.defaults`; confirm PENDING_VERIFY rollback still
   fires after crash + reset.
7. **E1003 touch I2C at 400 kHz.** Every Seeed source runs 400 kHz; our GT911
   inherits `BOARD_SHT4X_I2C_HZ 100000` (`touch_gt911.c:35-37`). Sticky
   already uses 400 kHz. Add `BOARD_TOUCH_I2C_HZ`.
8. **SD retry ladder** (extends the #34 fix). SenseCraft `sd_card.cpp:15-24`
   steps 1/2/4 MHz with 3 attempts each, 120 ms off / 300 ms on, and a 4 KB
   write probe (350-434) that rejects cards that mount but cannot be written
   (exFAT reports total 0). Ours retries at one frequency. Step
   `host.max_freq_khz` down on retry and log card type/size/freq.

## Tier 2: features or medium effort

9. **SY6974 charger IC at 0x6B** on every reTerminal: E1001/E1002 on a second
   I2C bus SDA 39 / SCL 40 (`reterminal_e1001/config.h:127-137`), E1003/E1004
   on 19/20. Read-only first: REG08 VBUS/CHRG status, REG09 faults
   (`pmic_sy6974.cpp:54-98`). Gives a `charging` flag, real battery presence
   (today `battery_present()` is compile-time true, so a cell-less unit on
   USB reads as a full battery through the OTA gate), and a mains signal for
   always-on eligibility. Seeed also sets ICHG 500 mA and disables the I2C
   watchdog; leave charge settings alone until the datasheet defaults are
   checked with an inline meter.
10. **Onboard LED**: E1001/E1002 GPIO6, E1003 GPIO16 (active low), E1004
    GPIO48 (polarity disputed: `LED_Control.ino:17` low, SenseCraft
    `reterminal_e1004.cpp:12` high). None of the pins are used by our
    headers. On during boot, slow blink in portal/BLE, fast blink on Wi-Fi
    failure, floating in sleep.
11. **E1001 fast + partial refresh.** TRMNL's E1001 build uses bb_epaper
    `EP75_800x480_GEN2`: partial for 1bpp images, full every 8 partials, fast
    when the interval >= 30 min (`TRMNL/src/display.cpp:55, 1541,
    1712-1721`). Register level in bb_epaper `bb_ep.inl:826-870` (fast: PSR
    0x1F, CDI 0x21 0x07, BTST 27 27 18 17, CCSET 0xE0 0x02, TSSET 0xE5 0x5A;
    partial: LUTs + CDI 0xA9 0x07 + PSR 0x3F; old frame to DTM1, new to
    DTM2, PTOU 0x92 + DRF). Ours is full-refresh only, DTM1 never written
    (`mono_spi.c:1030-1041, 1092-1096`), so overlay is not advertised on
    E1001. ~1.5 s vs 4 s and no black flash. Bench on both glass batches.
12. **E1001 mono vs 4-gray per frame** instead of two build kinds. TRMNL
    picks by bit depth per image (`display.cpp:1538-1568`). Needs wire
    format negotiation; unlocks 11 on the gray kind.
13. **GT911 gesture mode across deep sleep** (E1003, Sticky). SenseCraft
    writes 0x08 to 0x8046 then 0x8040 before sleep and arms ext0 active-high
    with pull-down (`lib/GT911/GT911.cpp:58-63`, `gt911_touch.cpp:71-99`).
    We leave the controller scanning (`touch_gt911.c:389-427`), mA-class for
    the whole sleep. Polarity flips in gesture mode, so this is a mode
    difference, not a contradiction of the verified active-low finding. The
    wake stub reads 0x8150; gesture mode reports at 0x814B. Bench current and
    tap-wake latency.
14. **E1003 SPI 10 MHz for image loads.** Seeed runs everything at 10 MHz
    (`GxEPD2_reTerminal_E1003.ino:81`); ours is 4 MHz
    (`seeed_reterminal_e1003.h:33`). A second spi_device at 10 MHz for the
    LD_IMG phase saves ~1.5 s per full paint. Bench reads at 10 MHz on the
    shared SD bus.
15. **Coredump partition** at 0x820000 (64 KB), enable
    `CONFIG_ESP_COREDUMP_ENABLE_FLASH`. SenseCraft declares 32 MB flash for
    all four reTerminals; `esptool flash_id` before using anything above 16 MB.
16. **Flasher: optional Wi-Fi pre-seed into NVS** on the factory-reset path
    only (`oshw/web/js/nvs.js` builds a 0x5000 image at 0x9000, ns + string
    keys). Ours never writes NVS; ns `wifi`, keys `ssid`/`pass`
    (`app_config.h:214-218`). Never in the parts-preserving path.

## Contradictions to settle on the bench

- **Sticky power latch.** TRMNL's Sticky build sets GPIO45 high (hold /
  DATA) then pulses GPIO46 (lock / CLK) at boot
  (`TRMNL_E1004/src/bl.cpp:835-841`). Our Sticky header has nothing on
  45/46. If the latch is real, a Sticky on battery only runs while the
  button is held. 30 s check: unplug USB, tap power, watch a full cycle.
  `power_latch.h` assumes a single level pin; this is a clocked latch.
- **E1003 VCOM**: Seeed 1400 mV (`GxEPD2_ED103TC2_1872x1404.cpp:45-47`,
  "different from the Waveshare"), ours 1500 from FastEPD
  (`seeed_reterminal_e1003.h:45-48`); driver supports 0 = keep the FPC
  value. Forced temperature 16 (Seeed, re-sent per refresh) vs our 14 once.
  Relevant to the #274 ghosting history. Bench gray ramp + ghosting at
  stored / 1400 / 1500.
- **E1001 register-LUT 4-gray plane polarity.** Seeed's Gray4 example sends
  white -> (0,0), black -> (1,1) (`GxEPD2_reTerminal_E1001_Gray4.ino:273-295`);
  ours sends the complement with identical LUT bytes (`mono_spi.c:803-810`),
  and `mono_spi.c:782-784` cites that example as OTP corroboration when it is
  a register-LUT example. Field evidence says ours works; fix the comment and
  run a ramp on legacy glass.
- **E1004 GPIO46 "POWER_EN".** SenseCraft drives it high at boot and passes
  it as the gauge's power-enable (`reterminal_e1004.cpp:21, 37-38`); we and
  TRMNL use only GPIO21. Confirm GPIO1 reads ~2 V with only 21 driven.
- **E1004 LED polarity** (see 10).
- **Mic rail GPIO38** (E1001/E1002/E1003, TPS22916 EN) and header rail
  GPIO46 (E1003) are left floating by us through sleep. Drive low +
  `gpio_hold_en`; a µA meter says whether it matters (Seeed's ~14 µA figure
  was taken without driving them).

## Already as good or better (no change)

Buzzer (GPIO45 PWM, ours has server patterns), buttons (ext1 ANY_LOW +
pull-up is identical, plus our hold classes and C3/classic backends), SHT4x,
battery ADC path (GPIO1, x2, eFuse-calibrated, 8-sample mean), SD shared-bus
discipline (our CS parking exceeds every example; Seeed's hub warns the
display may not work with a card inserted), OTA (ours signed + rollback +
battery gate; TRMNL is unsigned with rollback off), provisioning (portal +
BLE with pairing; neither Seeed path uses Improv), wake alignment, panel
init sequences for E1001 mono/OTP-gray, E1002 and E1004 (byte-identical to
Seeed's), E1003 transport (ours adds soft-attach, wedge recovery, partial
modes), rail power-down before sleep (Seeed leaves EN pins high).

Seeed-side bugs worth knowing: TRMNL's E1003 build omits E1003 from the
GPIO1 battery list so it reads the green button pin (`TRMNL/include/
config.h:171-177`); Seeed's own E1003 code disagrees with itself on the
VCOM sub-command (0x0002 vs 0x0001; ours uses 0x0001).
