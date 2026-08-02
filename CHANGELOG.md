# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Added SD-backed offline Albums for storage-capable displays: manifests and
  verified frame blobs sync incrementally, up to 32 photos play sequentially or
  in a shuffle bag with the requested repeat policy, and local photo wakes keep
  Wi-Fi off between independent server heartbeats. One-off Image or Dashboard
  frames interrupt the Album for a full interval and then playback resumes;
  reassignment or unbinding stops it on the next successful heartbeat.

### Fixed

- Stabilized reTerminal E1004 microSD reads after deep-sleep wake so cached
  Deck and Album frames remain usable instead of being treated as corrupt.
