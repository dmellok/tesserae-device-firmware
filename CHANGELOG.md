# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Fixed

- The protocol v2 push stream (SSE) now stays connected. The pump's short
  read timeout, which an idle stream hits between 25-second keepalives, comes
  back from the HTTP client as a negative "try again" code rather than zero
  bytes, and the pump took it for a dropped connection: it closed and
  reopened the stream about once a second, so no server push ever arrived
  that way and the one-second linger poll was quietly doing all the work.
  A timed-out read is now treated as idle. Affects every board with overlay
  (reTerminal E1003, XIAO EE03, reTerminal Sticky).

### Added

- Partial refresh on the Seeed reTerminal Sticky. The SSD1677 driver gains a
  windowed two-tone partial path: the controller's differential partial
  waveform driven from a driver-kept 1bpp shadow of what the glass shows,
  with the frame rect transposed onto the controller's scan and the temp /
  border selection restored before the next 4-gray paint. A patch takes about
  620 ms against 1.2 s for a full paint and leaves the rest of the panel
  alone (verified with the new `seeed-reterminal-sticky-overlaytest` env).
  The board advertises overlay, so tap echo, touch-v3 primitives, live value
  slots and frame patches all apply; the overlay, touch-v3 and proto2 drawing
  code learned the 2bpp (4-gray) framebuffer format on the way, with host
  tests for the new depth. Quality and hygiene requests fall back to a full
  4-gray paint, since the glass has no grayscale partial. Icons come from the
  bundled Phosphor font as on the E1003. Env `seeed-reterminal-sticky-touch3test`
  exercises the primitives offline.

### Fixed

- Always-on panels now answer taps and front-button presses. The stay-awake
  loop only serviced the digitiser on boards with partial refresh, and never
  polled the buttons at all, so a reTerminal Sticky set to stay awake ignored
  every tap and press (and their beeps). Every touch board now polls the
  GT911 while awake, presses are dispatched like a button wake, and the
  touch driver configures TP_INT as an input on the warm init path too, where
  it had been left unconfigured.

## [1.29.0] - 2026-09-04

### Added

- Touch on the Seeed reTerminal Sticky: the board header now declares its
  GT911 (own I2C bus on SDA3/SCL2, INT GPIO21, RST GPIO41, power-gated by
  TOUCH_EN on GPIO42), so touch wake, stroke capture and the RTC wake-stub
  quick-tap path build for the `seeed-reterminal-sticky` env, switched on by
  the server's `touch_enabled` config like the E1003. To get there the GT911
  driver and the wake stub take a dedicated `BOARD_TOUCH_I2C_*` bus (falling
  back to the SHT4x bus), a gated `BOARD_TOUCH_EN_PIN` held high across deep
  sleep, and an opt-in `BOARD_TOUCH_HOLD_RST`; the shared I2C accessor caches
  one bus per hardware port instead of one overall. Orientation verified on
  hardware with the `-selftest` env's corner-tap readout (no swap, Y
  inverted).

## [1.28.1] - 2026-09-03

### Fixed

- IT8951 panels (reTerminal E1003, XIAO EE03): a full paint that follows a
  partial pass in the same wake now runs an INIT clear first
  (dmellok/tesserae#274). The controller derives GC16 transitions from the
  previous image it holds, and after a touch wake that hard-reset it and
  loaded only the tap echo's rect, that reference no longer matched the
  glass; the pushed frame painted in the linger window then left the old
  frame's dark content showing through, while timed paints stayed clean.
  INIT drives every pixel to white regardless of the reference, so the GC16
  that follows starts true. Costs one extra flash and about 1.5 s, only on
  full paints that follow a tap.

## [1.28.0] - 2026-09-03

### Added

- New target: Waveshare 10.85-inch e-Paper HAT+ (G) driven by an ESP32-S3
  Zero. The 1360x480 dual-controller B/W/Y/R panel has a dedicated software-SPI
  driver, because its reference adapter requires chip-select to pulse for every
  byte. Frames use the native 2bpp `bwry_4` wire format (163200 bytes) and are
  split into two 170-byte halves per row. Envs `waveshare-1085g` and
  `waveshare-1085g-selftest`, hardware-catalog kind `waveshare_1085g`.
  Verified on physical hardware.

## [1.27.4] - 2026-09-02

### Changed

- A pushed patch whose rects cover 60% or more of the panel now paints with
  a full flashing refresh instead of a GC16 window pass
  (dmellok/tesserae#274). The window pass transitions pixels without an
  inversion flash, so residue from a DU pass written just before it (the
  tap echo's inverted rect) can survive at high contrast; at that coverage
  the full refresh costs about the same and leaves clean glass. Smaller
  patches keep the flash-free GC16 window pass.

## [1.27.3] - 2026-09-01

### Added

- New target: Seeed XIAO ePaper Display Board EE03 driving the 10.3"
  monochrome panel (ED103TC2, 1872x1404, 16-level grayscale via an onboard
  IT8951E/DX). Reuses the it8951_gray driver unchanged; the board header
  carries the EE03 pin map (single PWR_EN rail gate on GPIO43, battery sense
  on GPIO1 gated by GPIO6, buttons on GPIO2/3/5, SHT40 on GPIO42/41).
  Envs `seeed-ee03` and `seeed-ee03-selftest`, hardware-catalog kind
  `seeed_ee03`, BLE hardware code 11. Builds green; awaiting hardware
  verification.

### Changed

- Server-pushed frame patches now refresh with the full-quality GC16
  waveform instead of DU (dmellok/tesserae#274). A patch is a content
  change rather than a tap echo, so it is not latency-bound, but it was
  painted with the echo path's fast 2-level waveform, which leaves a
  visible ghost on high-contrast areas that stays on glass until the next
  timed full repaint. GC16 clears as it paints and costs about 20% more
  than DU on the E1003 (1564 vs 1330 ms in the 2026-07-27 mode sweep).
  Touch echoes and slot value redraws keep their fast waveforms.

## [1.27.2] - 2026-08-31

### Fixed

- Seeed XIAO EE05 (JD79676): `bwry_4` indices stream verbatim and the 0/1
  swap LUT that exchanged black and white is dropped.

## [1.27.1] - 2026-08-31

### Changed

- An onboarded panel no longer needs a manual RESET after a long Wi-Fi
  outage (dmellok/tesserae#270). Previously, once the short retry ladder was exhausted and
  the captive portal expired idle, the device deep-slept with no wake
  source, so a network that is switched off overnight left the panel on the
  pairing screen until someone pressed RESET. Now a device that has already
  onboarded (home-server token or relay pairing) sleeps for 30 minutes
  after an idle portal expiry, wakes, retries the stored network, and on
  another miss reopens the portal; the cycle repeats until the network is
  back, so the panel rejoins on its own within one cycle of the outage
  ending. A never-onboarded device keeps the old behaviour, since there the
  stored configuration itself is the likely problem. Relay-paired panels
  without a home-server token now also get the retry ladder instead of
  dropping to the portal on the first missed connect.

## [1.27.0] - 2026-08-28

### Added

- Synchronized wake support. When the server's `/status` response carries a
  `wake_at` epoch (wake alignment enabled for the device), the sleep duration
  is now derived from that absolute target at the moment of deep-sleep entry,
  so the frame fetch and panel refresh that happen after the response no
  longer push the wake late. The wall clock is already disciplined from the
  server's `Date` header on every response; the adjustment applied by each
  discipline, measured against the time since the previous one, now also
  feeds a drift estimate (EWMA, kept in RTC RAM) that trims the deep-sleep
  timer, so long aligned sleeps land on the wall-clock instant rather than
  drifting with the internal RC oscillator. Servers that don't send `wake_at`
  see no behaviour change.

- The M5Stack PaperS3 selftest sheet now paints a candidate grey matrix on its
  bottom half, derived from the epdiy project's waveform for this exact glass
  (ED047TC1, from-white mode, LGPL-3.0). Where the shipped matrix mixes darken
  and lighten codes per level, the candidate is a pulse-width ramp: each level
  darkens for a number of passes proportional to its distance from white, so
  the ramp cannot invert, only space unevenly. Normal display keeps the shipped
  matrix until the candidate is judged on glass and promoted. The grey driver
  now accepts a candidate with a different pass count from the shipped matrix;
  the sheet runs the longer waveform and floats the shorter half's pixels for
  the extra passes.

## [1.26.1] - 2026-08-27

### Fixed

- Fixed downloaded pictures silently never reaching the screen on displays
  without extra memory when an SD card is fitted (seen on the Xteink X4). The
  picture was downloaded and checked, then a second copy of it was made, and on
  a full memory that copy quietly failed; the display logged nothing and kept
  its old image. The copy is no longer made at all.

## [1.26.0] - 2026-08-27

### Added

- Added support for the Xteink X4, the 4.26" e-ink reader, turning one into a
  dedicated Tesserae panel. Pictures are black and white. Its one usable key is
  the power button: it wakes the display and advances to the next dashboard in a
  rotation. Needs a Tesserae hardware entry for `xteink_x4`; the display pairs
  without one but never receives a picture it can show.

## [1.25.1] - 2026-08-27

### Fixed

- Fixed the beep never sounding on button-only displays (reTerminal E1001 and
  E1002). The beep setting was only written to flash on displays that also
  have a touch screen, so a button-only display re-learned it from the server
  on every wake, always after the moment of the press it should have sounded
  for. The setting now persists on any display with a buzzer, and the press
  beeps from the second wake after it is turned on.

## [1.25.0] - 2026-08-26

### Changed

- M5Stack PaperS3: the retuned grey matrix is promoted to the shipped
  waveform; the candidate ramp is corrected and an ordered check sheet added.

## [1.24.0] - 2026-08-26

### Added

- The PaperS3 grey selftest paints a lettered, scrambled bar sheet so a
  tester's ranking is not biased by position.

### Changed

- Every board now advertises `can_stay_awake`; whether a panel stays awake is
  the operator's decision in the server, not a board property.

## [1.23.0] - 2026-08-26

### Added

- Added support for the Seeed XIAO ePaper Display Board EE05 with the 2.13"
  quadruple-colour panel (black, white, yellow and red; issue #29). This is the
  first four-colour panel here that takes a single interleaved colour buffer
  rather than separate ink planes, so it arrives with a new JD79676 panel
  driver. The panel's controller drives 128 columns but only 122 reach the
  glass; the server is told about the hidden columns and pads the frame, so the
  firmware streams it unchanged. Not yet confirmed on physical hardware; flash
  the `seeed-ee05-selftest` build first. Its test pattern doubles as the
  bring-up questionnaire: which edge swallows the hidden columns, whether the
  colour order is right, and which way the rows run can all be read off a
  photo of the glass.

## [1.22.1] - 2026-08-25

### Fixed

- Fixed physical button presses not beeping on displays where the beep was
  turned on. Touches sounded correctly; presses did not. A press is handled
  almost at the top of boot, before the display or the network exist, and the
  setting it reads lives in flash that had not been initialised yet, so it read
  as switched off. Flash now comes up first and a press reads the real setting.

## [1.22.0] - 2026-08-24

### Added

- Displays in the reTerminal E series can now beep when you touch them or press
  a button. E-ink takes seconds to repaint, so until it does there is nothing to
  tell the person standing at the display that it heard them. The buzzer on the
  board sounds the moment the input registers, before the server is contacted at
  all, and the tone and volume are yours to choose from the display's settings
  in Tesserae. Off unless you turn it on. Needs Tesserae v0.362.0 or later.

## [1.21.0] - 2026-08-24

### Fixed

- A touch display that is already awake now picks up a whole new dashboard
  rather than sleeping through it. After a tap the display stays awake for its
  touch linger window, watching for small changes it can paint as rectangles,
  but a change too big for that arrives as a new frame instead, and nothing
  told the display to go and get one. Tapping a button that rewrites most of
  the screen therefore did nothing visible until the next scheduled wake, up to
  a full sleep interval later. The server now says when the frame on screen has
  been superseded (Tesserae v0.360.0), and the display fetches it before the
  window closes. Mains-powered displays bring their next scheduled check
  forward instead, so the panel's own refresh limit still governs how often the
  screen is driven. Displays showing a Deck page or Album photo from their own
  card are left alone.

## [1.20.0] - 2026-08-24

### Added

- Added support for the M5Stack PaperS3, the 4.7" touch e-ink handheld. This is
  the first display here whose screen has no controller chip of its own: the
  ESP32-S3 drives the glass directly and the sixteen grey levels come from a
  waveform table in the firmware rather than from the panel. Greyscale tuning is
  therefore a firmware change rather than a panel setting, and partial refresh is
  not available yet, so this display does not offer tap feedback or on-device
  touch controls. The touch digitiser is present but not yet enabled: the board
  gives the processor no touch reset line, which the existing touch driver needs.
  Not yet confirmed on physical hardware; flash the `m5stack-papers3-selftest`
  build first and check the sixteen grey bands are evenly spaced.

## [1.19.0] - 2026-08-23

### Added

- The reTerminal Sticky reports its battery through the onboard BQ27220
  fuel gauge (#27).

## [1.18.0] - 2026-08-23

### Added

- Added support for the Seeed reTerminal Sticky (SSD1677, 4-level
  grayscale), verified on hardware, built and published by CI (#25, #26).

### Fixed

- E1001 4-gray probe: sample the data line late, sanity-gate the answer, and
  remember it across wakes (#24).

## [1.17.1] - 2026-08-22

### Fixed

- Album sync reports a failing sync instead of showing "syncing"
  indefinitely (#23).

## [1.17.0] - 2026-08-22

### Added

- Always-on mode for mains-powered panels: the panel stays awake between
  refreshes instead of deep-sleeping (#22).

## [1.16.4] - 2026-08-20

### Fixed

- reTerminal E1001 4-gray: the firmware asks the panel which waveform it
  carries and drives it the way Seeed's own firmware does (#19).

## [1.16.3] - 2026-08-19

### Fixed

- Fixed the XIAO 7.5" ePaper Panel reporting an empty battery. It has a cell
  but no way to measure it, and it was sending a literal zero, which reads as
  flat rather than unknown. It now omits the reading entirely, so nothing shows
  a permanently empty battery for that display.

- Fixed over-the-air updates never installing on the XIAO 7.5" ePaper Panel.
  Updates are held back when a display is low on charge, and the unmeasurable
  battery above counted as empty, so every update was deferred indefinitely.
  Displays that cannot measure their charge now skip that check.

## [1.16.2] - 2026-08-19

### Fixed

- Fixed the browser flasher warning that a display was being given firmware
  built for the wrong processor. Every published image was labelled ESP32-S3
  regardless of the board, so the XIAO 7.5" ePaper Panel, which uses an
  ESP32-C3, was advertised incorrectly. The firmware itself was always correct,
  and anyone who flashed past the warning got a working display.

## [1.16.1] - 2026-08-19

### Fixed

- The setup screen now shows the Wi-Fi password for the display's own setup
  network, next to the network name. It named a network while giving no way to
  join it, leaving the QR code as the only route in, and many phones do not act
  on a Wi-Fi QR code. The password is also written to the serial log.

## [1.16.0] - 2026-08-19

### Added

- Added support for the Seeed XIAO 7.5" ePaper Panel, the all-in-one display
  built around a XIAO ESP32-C3. This is a different board from the XIAO ePaper
  7.5" kit already listed, which pairs an ESP32-S3 with a separate driver
  board, and the two are not interchangeable. Setup is through the captive
  portal, since the panel has no Refresh button, and it does not report battery
  level because the hardware provides no way to measure it. Not yet confirmed
  on physical hardware.

## [1.15.0] - 2026-08-19

### Changed

- Added a line to the setup screen on displays that support it, pointing out
  that holding Refresh switches to app setup over Bluetooth. The captive portal
  page already said so, but only once you had joined the Wi-Fi network the app
  was meant to save you from.

- Holding Refresh again for 3 seconds during Bluetooth setup now returns to the
  captive portal instead of leaving the display on the pairing screen for the
  rest of its five-minute window. The screen says so.

### Fixed

- Fixed the captive portal losing its automatic pop-up after a Bluetooth
  session. Returning to the portal left the previous DNS responder's socket
  open, so the new one could not start and the setup page had to be reached by
  typing its address.

## [1.14.0] - 2026-08-19

### Added

- Added a stable hardware identifier to Bluetooth discovery so Companion can
  show a nearby display's brand and model before connecting.

- Added a five-minute Bluetooth setup and maintenance mode for nearby Companion
  apps. On supported Seeed displays with a Refresh button, new displays open
  the captive-portal AP first and switch to Bluetooth after a 3-second Refresh
  hold; existing displays use the same gesture. The portal explains the app
  option, and Bluetooth timeout returns to the AP. Users can scan an ephemeral
  on-screen QR code or use a six-digit LE Secure Connections passkey to scan
  and test Wi-Fi, connect a Tesserae server, inspect bounded diagnostics,
  reboot, clear Wi-Fi settings, or factory reset. Configuration is saved only
  after both Wi-Fi and server checks pass, and the existing captive portal
  remains the fallback.

### Changed

- Aligned the Bluetooth setup screen with the captive-portal design, centering
  the Tesserae branding, QR code, Companion scan prompt, and fallback passkey.

- Limited the NimBLE setup stack to supported Seeed displays with a Refresh
  button. Other boards retain captive-portal setup without carrying an
  unreachable Bluetooth implementation.

### Fixed

- Fixed the nearby-network scan Companion runs during Bluetooth setup, which
  asked the Wi-Fi driver for a faster per-channel dwell than it accepts while
  Bluetooth is on. The driver rejected the request and warned, and the scan
  could return fewer networks than are really in range. Scans now use the
  driver's own timing whenever Bluetooth is active, and keep the faster dwell
  for the captive portal, where the radio is not shared.

- Increased the Bluetooth host task stack used by authenticated setup so
  connection key derivation cannot corrupt NimBLE state before the first
  Companion command.

- Serialized Bluetooth worker, host-loop, task, and memory shutdown so setup,
  Wi-Fi clearing, and factory-reset sessions no longer corrupt NimBLE queues or
  reboot unexpectedly during the next Companion connection.

- Prevented AP-first onboarding from rebooting when Companion requests its
  first Bluetooth Wi-Fi scan after switching from the captive portal.

- Allowed the same on-screen Bluetooth QR code to reconnect throughout its
  five-minute maintenance session, while deriving a new encrypted channel for
  every connection so captured commands cannot be replayed after reconnecting.

- Kept Bluetooth setup and maintenance text and QR codes inside their assigned
  regions across portrait and landscape displays, with shorter instructions
  and text-width-aware font scaling for each panel size.

- Returned a registered display to Bluetooth maintenance first after Companion
  clears its Wi-Fi, preserving its server identity and avoiding an unnecessary
  new pairing code; brand-new and factory-reset displays remain AP-first.

- Made a 3-second Refresh hold enter Bluetooth maintenance consistently during
  both deep-sleep wake and active button windows; short presses still perform
  an ordinary refresh after release.

- Allowed authenticated Bluetooth maintenance to repair only Wi-Fi while
  preserving the exact saved server URL and device token, avoiding an
  unnecessary new device pairing code.

- Allowed maintenance command acknowledgements and the Bluetooth disconnect to
  finish before rebooting, so Companion can visibly confirm restart and reset
  actions.

- Forced a full server-frame repaint after leaving Bluetooth maintenance so an
  unchanged `304` response cannot leave the setup QR code on the e-paper panel.

- Advertised the ephemeral Bluetooth session ID so Companion can distinguish a
  newly started maintenance session from an earlier one on the same display.

- Prevented reTerminal E1004 from rebooting while painting the Bluetooth setup
  QR screen, which left Companion seeing a stale advertisement and timing out
  while connecting.

- Connected saved Wi-Fi during Bluetooth maintenance so diagnostics report the
  display's live IP address and signal strength instead of blank values.

## [1.13.0] - 2026-08-12

### Fixed

- reTerminal E1001 legacy glass: the 4-gray LUTs are compensated for
  temperature, the plane encoding is derived from the hardware rather than
  assumed, the LUT drive time is stretched, and the border artefact is fixed.
  Panel refresh now confirms the refresh started before powering the panel
  down.

## [1.12.0] - 2026-08-06

### Added

- Added SD-backed offline Albums for storage-capable displays: manifests and
  verified frame blobs sync incrementally, up to 32 photos play sequentially or
  in a shuffle bag with the requested repeat policy, and local photo wakes keep
  Wi-Fi off between independent server heartbeats. One-off Image or Dashboard
  frames interrupt the Album until its next scheduled playback deadline without
  repeatedly postponing that deadline; reassignment or unbinding stops it on
  the next successful heartbeat.

### Fixed

- Stabilized reTerminal E1004 microSD reads after deep-sleep wake so cached
  Deck and Album frames remain usable instead of being treated as corrupt.

- Allowed the same rendered photo to occupy multiple Album positions without
  rejecting the complete collection manifest.

- Kept transient short SD reads from being mistaken for corrupt cached Deck or
  Album frames, and prevented repeated network frames from starving Album
  playback indefinitely.
