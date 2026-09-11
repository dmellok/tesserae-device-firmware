# Testing plan: Seeed-review batch (2026-09-11)

What was built is listed under [Unreleased] in CHANGELOG.md; the source
review is notes/seeed-oshw-review-2026-09-11.md. Everything here is
compiled and wired; nothing below has been measured yet except where marked.
Order is by risk to shipped units: the items at the top change every wake on
every reTerminal, the ones at the bottom are opt-in flags.

Bench kit: E1002 (on the desk), E1001 both glass batches, E1003, E1004,
Sticky; a USB inline current meter or a bench supply with current readout
(sleep figures need a µA range: Nordic PPK2, Otii, or a Joulescope; a USB
meter is enough for the paint-phase numbers); a 16 GB FAT32 card; a laptop
on the same Wi-Fi as a Tesserae server.

Serial: `pio device monitor -b 115200` on `/dev/cu.wchusbserial*` (reTerminal
carriers use the CH340; the native-USB XIAO boards need the usbjtag envs and
will NOT light-sleep while the monitor is attached, by design).

## 0. Regression gate (every board you own, 15 min each)

Flash the production env for the board, leave the Wi-Fi config in NVS.

- Cold boot: logo splash paints, `clock seeded from the board RTC:` line
  appears on reTerminals (if it does not, see 3), device connects, first
  frame paints, `paint done in N ms (K light-sleep naps)` with K > 0 on the
  reTerminals, device sleeps. Timer wake: frame or 304, sleep again.
- Buttons still wake and act (refresh / prev / next). Touch still wakes
  (E1003, Sticky) with the default build.
- Status in the server's device page shows a sane battery percent (not 0,
  not stuck) and, on reTerminals, the new `vbus` / `charging` fields in the
  raw status (server logs or the device debug view).
- Nothing in the boot log says `No core dump partition found` on a 16 MB
  board (it will on a unit flashed before this change until it is USB
  reflashed; that is expected and harmless).

Pass = identical behaviour to 1.34.0 plus the new log lines. Any driver that
hangs at "painting" is item 1.

## 1. Light sleep during the paint (all boards; measure on E1002 + E1003)

Why first: touches every driver's BUSY loop.

   Done so far (2026-09-11, E1002 soak build, no meter): the cold-boot splash
   refresh takes 29.2 s wall-clock, identical to 1.34.0, and the FreeRTOS
   tick advanced only 315 ms during it, i.e. the SoC was in light sleep for
   ~99 percent of the paint. Earlier attempts found two bugs now fixed in
   busy_sleep.c (nap floor 200 ms; esp_sleep_enable_gpio_switch(false)).
   Note: log timestamps do not advance during light sleep, so serial times
   inside a paint read short; use wall-clock or the `paint done in N ms`
   line (esp_timer, compensated).

1. E1002, USB meter inline, production build. Trigger a paint (change the
   dashboard or press Refresh). Read the average current during the ~25 s
   refresh. Compare with 1.34.0 on the same unit (or `-DEPD_NO_LIGHT_SLEEP`
   build flag, which is the same code with the naps off).
   Expect: tens of mA down to low single-digit mA during the refresh; the
   paint takes the same wall time (±0.5 s); K naps in the paint log line is
   in the tens to hundreds.
2. E1003: no naps by design (the IT8951's long wait polls LUTAFSR over SPI
   and HRDY waits are per-row); confirm the paint log says 0 naps and the
   10 MHz data phase (item 7) works. Also tap the panel mid
   paint: the tap should still register after the paint (it is queued, not
   lost) or be cleanly ignored; note which.
3. E1004 (dual-controller, two BUSY lines) and Sticky: one paint each,
   confirm naps > 0 and the image is complete.
4. Portal and BLE: open the captive portal (factory reset or clear Wi-Fi)
   and confirm the portal splash paints with naps = 0 (the gate must forbid
   sleep while the soft-AP is up); same for a BLE maintenance splash.
5. Serial logs during the refresh may lose characters on the CH340 boards;
   that is expected (TRMNL notes the same). If it is a problem for bench
   work, build with `-DEPD_NO_LIGHT_SLEEP`.

Fail modes to watch: a paint that never completes (BUSY level wrong for that
driver), a Wi-Fi disconnect during a linger/overlay wake (gate missed a
start site), an image with a torn band (SPI transaction interrupted; should
be impossible since naps only happen while waiting).

## 2. Battery tables and the read cache (E1001/E1002, E1003, E1004)

1. With a meter on the cell (or a bench supply at set voltages through the
   battery connector), compare the reported percent at 3.5 V, 3.7 V, 3.9 V,
   4.1 V against Seeed's tables (E1001: 3.7 V is ~60 percent, 4.11 V is
   100). Old firmware read 30 percent at 3.7 V.
2. Confirm the status post mV equals the pre-radio reading in the log
   (`battery_goodbye` line) rather than a lower under-TX figure.
3. Low-battery gate: AWAKE_BATTERY_MIN_PCT 15 now lands at ~3.43 V on the
   E1001 (was ~3.50 V). Decide whether that floor still protects the pack;
   if not, raise the percent.

## 3. Board RTC (all reTerminals)

   Done so far (2026-09-11): E1002 seeded on the first cold boot (its RTC
   already held a valid time). E1001 reported VL set (backup had run low),
   was written back after the first Date discipline, and seeded the next
   cold boot. So the write-back path and the VL gate both work.

1. Cold boot with the coin cell fitted: expect `clock seeded from the board
   RTC: <UTC>` before Wi-Fi. If instead nothing prints, either the cell is
   flat (VL flag set; Seeed says the chip resets on every power loss without
   a CR1220) or the chip did not answer. Log at `rtc` tag with `-DLOG_LOCAL_LEVEL`
   or check `rtc_pcf8563_present()` in the sdtest build.
2. Pull the battery and USB for a minute, boot again: the seeded time should
   be within a few seconds of real UTC.
3. After a successful fetch, the Date header disciplines the clock; if it
   moved more than 2 s a write-back happens. Force it by setting the RTC
   wrong (there is no CLI; easiest is a unit that has never been synced),
   then check the next cold boot seeds a correct time.
4. Quiet hours / sleep-through: with the server's quiet window active, a
   cold boot during the window should now respect it on the first wake.

## 4. Charger readout (all reTerminals)

   Done so far (2026-09-11): E1002 answers on GPIO39/40 (vbus=1 charging=1);
   E1001 the same. A single REG09 read returned a stale latched BAT_FAULT on
   an E1001 with a healthy cell; the driver now reads it twice (Seeed does
   too) and reports the live value.

1. E1002 on USB with a cell: status shows `vbus: true, charging: true`
   until full, then `charging: false` with vbus still true. On battery only:
   `vbus: false`. If the fields are absent the SY6974 did not answer:
   E1001/E1002 talk to it on a SECOND I2C bus (GPIO39/40); confirm with an
   I2C scan build or a logic probe on those pins.
2. E1003/E1004: same, on the main bus (19/20).
3. USB with NO cell: `battery_fault` may appear (NTC open). Note what the
   ADC path reports for mV in that state; the OTA low-battery gate still
   uses the ADC and will think a cell is full. Wiring `battery_present()` to
   the charger is a follow-up once this reading is characterised.

## 5. LED (all reTerminals; E1004 polarity)

1. Cold boot: LED on until the first frame paints, then off. Timer wake:
   LED stays dark throughout.
2. Captive portal: slow blink (100 ms on per second). BLE session: same.
   Both stop when the session ends.
3. Wi-Fi failure on an onboarded unit: ~1.5 s of fast blink, then sleep.
4. Deep sleep: LED dark; if it glows faintly, the pin is not hi-Z (check
   `led_prepare_sleep`).
5. E1004: if the LED is ON when it should be off, the polarity is active-
   high (SenseCraft's reading); flip `BOARD_LED_ACTIVE_LOW` in
   boards/seeed_reterminal_e1004.h.

## 6. Sleep current and rail parking (E1001/E1002/E1003)

Needs a µA-range meter on the battery connector.

1. Baseline: 1.34.0 build, touch disabled, one full wake then deep sleep;
   read the settled sleep current after ~30 s. Seeed's figure for the same
   boards is ~14 µA.
2. This build: same measurement. The changes that can move it: GPIO38 (mic
   rail) and E1003 GPIO46 driven low + held, PCF8563 CLKOUT off, LED hi-Z.
   Record the delta; if it went UP, unhold one pin at a time.
3. Skip-validate: time from the wake timer to the `boot;` line before and
   after (serial timestamps). Expect tens to a few hundred ms less. Then
   confirm rollback still works: flash an OTA image that panics before
   marking itself valid, reset, check the bootloader returns to the previous
   slot.

## 7. IT8951 10 MHz data phase and 400 kHz touch bus (E1003)

1. Full paint: log line for the data clock; paint time should drop by
   ~1.5 s versus 1.34.0. Look for any tearing, wrong-gray bands or an
   `LD_IMG` timeout; if so, drop `EPD_IT8951_DATA_HZ` to 8 MHz and retry.
   Reads (GET_DEV_INFO, LUTAFSR) still run at 4 MHz and must keep working
   with a card in the SD slot (shared MISO).
2. Touch at 400 kHz: 50 taps across the panel, no I2C NACK / timeout lines,
   coordinates unchanged.

## 8. SD retry ladder (E1002 with the reporter's failure in mind, #34)

1. Working card: mount log now reads `mounted /sdcard (... MB) SDHC/SDXC
   <name> @ 20000 kHz` on the first attempt, no ladder.
2. A slow or marginal card (old 2 GB SDSC, a cheap no-name): expect retries
   at 4000 then 1000 kHz. If a card only mounts at the lower clock, consider
   lowering the E1001/E1002 default like the E1003/E1004.

## 9. E1004 bounded BUSY (E1004)

1. Normal paint: complete image, ~50 ms slower per paint (the ready filter).
2. Fault injection: pull the panel FPC (or hold BUSY low with a jumper)
   during a paint. Expect `timeout` log lines, then POF / DSLP / EN-low and a
   normal deep sleep instead of a hang; the next paint after reconnecting
   works.

## 10. Sticky power latch (Sticky, battery fitted)

Unplug USB. Tap the power button and let go. Expect the unit to boot, fetch,
paint and go through a full sleep/wake cycle on battery. If it dies the
moment the button is released, the latch pins or the clock edge are wrong
(TRMNL's sequence is 45 high, 46 low then high). If it already worked on
battery before this change, the latch was never needed; either way record
it in boards/seeed_reterminal_sticky.h.

## 11. E1001 fast + partial refresh (opt-in; both glass batches)

   Done so far (2026-09-11, the user's E1001, OTP batch per the NVS probe
   cache): bars full 4847 ms; partial band 3 flip 1214 ms total (894 ms
   refresh, GEN2 register LUTs); FAST full 1884 ms total (1629 ms refresh,
   OTP waveform). Serial side passes; the visual judgement (no flash, no
   ghost, border) is the user's. Legacy batch still untested.

Build `seeed-reterminal-e1001-partialtest`. Watch the serial log for which
branch the probe chose (`built-in OTP waveform` vs `register LUTs`; erase
NVS to re-probe a unit that ran the gray build before).

1. Bars paint with the normal flashing full refresh.
2. `SELFTEST partial`: band 3 turns black with NO black flash across the
   panel, other bands untouched, no ghost of the old band state, border ring
   not darkened. Record the time.
3. `SELFTEST fast`: band 4 turns white in ~1.5 s with no flash. Record.
4. If the partial does nothing on the OTP-batch unit, rebuild with
   `-DEPD_MONO_PARTIAL_FALSE_DIFF=1` (TRMNL's exact GEN2 behaviour).
5. Leave the unit asleep for an hour; the border must not darken (the driver
   restores CDI before sleep; TRMNL does it differently).
6. Only when both batches pass: consider a production env with
   `-DEPD_MONO_PARTIAL`, which makes the E1001 advertise overlay to the
   server. Test overlay/touch3-style slot updates end to end before any
   release carries that flag.

## 12. GT911 gesture sleep (opt-in; E1003 then Sticky)

Build the E1003 env with `-DTOUCH_GESTURE_SLEEP`, touch enabled on the
server.

1. Sleep log: `gesture sleep: INT=0 before sleep (idle)`. If it says still
   active, the mode switch did not take and the device will wake at once.
2. Sleep current with touch enabled: default build vs this build. Expect the
   mA-class draw to drop to sub-mA.
3. Wake: single tap, double tap, swipe. `wakeup_cause=6` (EXT0) in the boot
   line, then `leaving gesture mode: hardware reset`. The key question is
   whether a single tap wakes at all: the GT911's flash config decides which
   gestures count, and the standard id table has no single-tap id. If only
   double tap / swipe wake, this stays opt-in or needs the controller's
   config rewritten.
4. Buttons still wake via ext1 alongside ext0.
5. Coordinate after a gesture wake: the stub yields no point and the reset
   eats ~120 ms, so only a finger still down ~1.2 s later gives a point.
   Check the quick-tap path logs `no point readable` cleanly.

## 13. Contradictions still open (no code change, bench only)

- E1003 VCOM: Seeed uses 1400 mV, we use 1500. Build the E1003 selftest
  with `-DEPD_VCOM_MV=1400` and `-DEPD_VCOM_MV=0` (trust the FPC value),
  compare the 16-gray ramp and residual ghosting against 1500. Relevant to
  #274.
- E1003 forced temperature 16 (Seeed) vs 14 (ours): fold into the same run.
- E1001 register-LUT 4-gray plane polarity: Seeed's example sends the
  complement of ours. Run the gray ramp selftest on a LEGACY-batch unit
  and confirm black..white order; if inverted, the plane bits in mono_spi.c
  (register-LUT path only) need flipping.
- E1004 GPIO46: SenseCraft drives it high as a power enable. With only
  GPIO21 driven, confirm GPIO1 reads ~2 V (half the cell) during the ADC
  read; if it reads 0, the divider is behind 46 and battery telemetry on
  the E1004 has been wrong.

## Release gate

Ship when 0 passes on every board you own and 1, 2, 3, 5 pass on at least
one reTerminal. 4, 6, 7, 8, 9 can land with numbers recorded here. 10-13
stay opt-in or open until their bench line is filled in.
