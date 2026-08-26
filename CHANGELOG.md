# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Added support for the Xteink X4, the 4.26" e-ink reader, turning one into a
  dedicated Tesserae panel. Pictures are black and white. Its one usable key is
  the power button: it wakes the display and advances to the next dashboard in a
  rotation. Needs a Tesserae hardware entry for `xteink_x4`; the display pairs
  without one but never receives a picture it can show.
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
- Displays in the reTerminal E series can now beep when you touch them or press
  a button. E-ink takes seconds to repaint, so until it does there is nothing to
  tell the person standing at the display that it heard them. The buzzer on the
  board sounds the moment the input registers, before the server is contacted at
  all, and the tone and volume are yours to choose from the display's settings
  in Tesserae. Off unless you turn it on. Needs Tesserae v0.362.0 or later.
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
- Added support for the Seeed XIAO 7.5" ePaper Panel, the all-in-one display
  built around a XIAO ESP32-C3. This is a different board from the XIAO ePaper
  7.5" kit already listed, which pairs an ESP32-S3 with a separate driver
  board, and the two are not interchangeable. Setup is through the captive
  portal, since the panel has no Refresh button, and it does not report battery
  level because the hardware provides no way to measure it. Not yet confirmed
  on physical hardware.
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
- Added a line to the setup screen on displays that support it, pointing out
  that holding Refresh switches to app setup over Bluetooth. The captive portal
  page already said so, but only once you had joined the Wi-Fi network the app
  was meant to save you from.
- Holding Refresh again for 3 seconds during Bluetooth setup now returns to the
  captive portal instead of leaving the display on the pairing screen for the
  rest of its five-minute window. The screen says so.

### Fixed

- Fixed physical button presses not beeping on displays where the beep was
  turned on. Touches sounded correctly; presses did not. A press is handled
  almost at the top of boot, before the display or the network exist, and the
  setting it reads lives in flash that had not been initialised yet, so it read
  as switched off. Flash now comes up first and a press reads the real setting.
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
- Fixed the XIAO 7.5" ePaper Panel reporting an empty battery. It has a cell
  but no way to measure it, and it was sending a literal zero, which reads as
  flat rather than unknown. It now omits the reading entirely, so nothing shows
  a permanently empty battery for that display.
- Fixed over-the-air updates never installing on the XIAO 7.5" ePaper Panel.
  Updates are held back when a display is low on charge, and the unmeasurable
  battery above counted as empty, so every update was deferred indefinitely.
  Displays that cannot measure their charge now skip that check.
- Fixed the browser flasher warning that a display was being given firmware
  built for the wrong processor. Every published image was labelled ESP32-S3
  regardless of the board, so the XIAO 7.5" ePaper Panel, which uses an
  ESP32-C3, was advertised incorrectly. The firmware itself was always correct,
  and anyone who flashed past the warning got a working display.
- The setup screen now shows the Wi-Fi password for the display's own setup
  network, next to the network name. It named a network while giving no way to
  join it, leaving the QR code as the only route in, and many phones do not act
  on a Wi-Fi QR code. The password is also written to the serial log.
- Fixed the captive portal losing its automatic pop-up after a Bluetooth
  session. Returning to the portal left the previous DNS responder's socket
  open, so the new one could not start and the setup page had to be reached by
  typing its address.
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

## [Unreleased]

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
