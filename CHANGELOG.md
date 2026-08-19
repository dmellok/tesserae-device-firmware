# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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
