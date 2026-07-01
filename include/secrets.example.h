/*
 * Local credential overrides — copy to secrets.h (which is git-ignored)
 * and uncomment the lines you want to bake into the build.
 *
 * Precedence on each wake:
 *   1. Values in NVS (set via the captive portal)  -- always win if present
 *   2. Values defined here                          -- fallback for empty NVS
 *   3. Otherwise                                    -- captive portal triggers
 *
 * Use these to bypass the portal during development -- no NVS, no phone-tap
 * dance every time you flash a fresh board. Leaving any of them undefined is
 * fine; the portal will collect whatever's missing.
 *
 * IMPORTANT: secrets.h itself must NEVER be committed. .gitignore is wired
 * to ignore it; double-check before pushing.
 */
#pragma once

/* ---- WiFi ---------------------------------------------------------- */
// #define WIFI_DEFAULT_SSID  "your-network"
// #define WIFI_DEFAULT_PASS  "your-password"     /* "" for open networks */

/* ---- Tesserae REST server ------------------------------------------ */
// Base URL of your Tesserae server. The device registers over REST at
// <server_url>/api/v1/device/ and the admin approves it in Settings -> Devices.
// #define REST_DEFAULT_SERVER_URL   "http://192.168.1.50:8765"   /* https:// also works */
//
// Optional: opt into strict admin-gated register instead of zero-touch discover.
// #define REST_DEFAULT_PAIRING_CODE "abc123"

/* ---- Dev mode ------------------------------------------------------ */
/* The firmware auto-detects a connected USB host (laptop) and skips deep
 * sleep in that case, so you usually DON'T need DEV_DISABLE_SLEEP just
 * for development. Define it only when you want to force the loop without
 * a USB host (e.g. wall-mounted install on a USB charger -- the charger
 * doesn't emit SOF packets so auto-detection treats it as battery). */
// #define DEV_DISABLE_SLEEP
// #define DEV_LOOP_INTERVAL_S 10                /* defaults to 10 */

/* Opposite of DEV_DISABLE_SLEEP: skip the USB-host auto-detect and ALWAYS
 * deep-sleep, even when plugged into a laptop. Useful for exercising the
 * battery wake path (proper esp_deep_sleep with the configured interval +
 * wake_reason "timer") while keeping serial / flash access. The CH343 UART
 * port stays enumerated across sleeps; esptool's RTS reset still wakes the
 * chip for re-flashing. Mutually exclusive with DEV_DISABLE_SLEEP. */
// #define DEV_FORCE_SLEEP
