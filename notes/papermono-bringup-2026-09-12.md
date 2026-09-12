# M5Stack PaperMono bring-up, 2026-09-12

Unit: PaperMono Pro, registered as `papermono_44702c` on the LAN server
(192.168.50.125:8765, 0.412.2). Board header `boards/m5stack_papermono.h`.

## Verified on the bench

- Panel: expander (M5IOE1 @0x4f, uid 0x13a0) powers/resets the SSD1677; the
  four-grey ramp needs `EPD_SSD1677_GRAY_TEMP 0x5A` and
  `EPD_SSD1677_GRAY_MID_SWAP 1` (the Sticky's values paint static here).
  Orientation = Sticky's transpose, wedge top-left. Dashboard frames upright,
  correct greys, 3.9 s full paint.
- Onboarding: captive portal, Wi-Fi, discover/register, REST frame fetch.
- Buttons: KEY1 (GPIO2, refresh slot reported as "left") and KEY2 (GPIO3,
  right) both repaint; KEY2 wakes from deep sleep via ext1.
- Touch: FT6336G (chip 0x64, vendor 0x11, fw 0x13) on the selftest; corner
  taps -> (34,63) top-left and (458,762) bottom-right, so no swap/inversion.
- Partial refresh (overlay selftest): tiles invert and the digit slot counts
  cleanly, ~0.78 s a window, rest of the page undisturbed.
- microSD: 16 GB card mounts at 20 MHz through the expander rail; sdtest
  write/digest/read-back pass; card-detect polarity right (mount happens).
- Frontlight: `frontlight_pct` 30 from server config -> PM1 PWM0, log shows
  "frontlight 30%" every boot. Whether it visibly lit was never confirmed.
- PMIC battery: VBAT 4.02-4.08 V read fine on boots that only READ the PM1.

## Open: the repaint loop

Symptom: with the production build, the panel repainted the same dashboard
every few seconds and the USB console went silent (port still enumerated,
zero bytes; esptool could not connect). Timeline of toggles, all on USB:

1. Server 0.412.2, Touch input ON + Frontlight 30 -> loop.
2. Touch OFF -> loop stopped (after ~1 min).
3. Touch ON -> loop again. (Stay awake was ON at some point here.)
4. Touch OFF, Stay awake ON -> still looping. Stay awake OFF -> still looping.
5. Card OUT + power cycle -> stopped. Card back IN -> no loop.

After step 5 every boot logs: card mounted, proto2 manifest restored from SD
(0 regions), touch3 spec 4f53cda18c2baa0c empty, frame 304, no paint.

Leading hypothesis (not yet reproduced under console): touch3 spec restore.
`touch3_boot()` re-adopts the spec cached on SD at every boot; `adopt_spec`
sets `s_want_repaint` whenever `!had_controls && n_prims > 0`, and
`s_have_spec` is always false at boot, so a cached spec WITH primitives forces
a full repaint on every boot/wake (main.c ~2664, only when the frame is 304).
With Touch input on, the server may serve a spec with primitives for the
Big Glance page; on USB the board restarts every 10 s -> repaint every cycle.
Turning touch off makes the server return an empty spec, which overwrites the
SD copy on the next poll -> loop ends. Pulling the card removed the cached
spec's effect. If this is right it also hits the Sticky on every timer wake
whenever a touch page is on glass; the fix is to persist "controls already on
glass" (write_current already records the layout digest; compare against it
before flagging the repaint) rather than deriving it from RAM state.
Unexplained by this: why the console died (light sleep? the busy_sleep gate
keys off usb_serial_jtag_is_connected), and the always-on behaviour in step 4.

Fixes landed but NOT yet bench-verified:
- touch_ft6336 prepare_sleep: RTC pull-up on INT (GPIO4), 300 ms settle,
  withhold the touch wake for the cycle if INT still reads low.
- m5pm1: sample VBAT before the first PM1 write of a boot (reads after a
  write came back ~120 mV on every boot with the frontlight/LED code).

## Tomorrow

1. Flash `m5stack-papermono` with the console attached, Touch OFF, Stay awake
   OFF, card IN: confirm VBAT reads sane and boots stay 304/no-paint.
2. Turn Touch ON while capturing: expect `GET /frame/spec` to return prims;
   watch for "touch spec gained controls; forcing a repaint" every boot.
   That confirms the hypothesis; fix in touch3_run.c adopt_spec/touch3_boot.
3. Then tap test on the live path (touch wake from deep sleep on battery),
   frontlight visible check, buzzer (GPIO42 LEDC; M5's own demo drives it the
   same way, so if silent suspect volume/duty or the enabled flag not
   reaching the device), stay-awake under console.
4. Board is currently on `m5stack-papermono-selftest` (halts after the ramp).
