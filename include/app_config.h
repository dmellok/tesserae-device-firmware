/*
 * Project-wide tunables. Edit here, not scattered across .c files.
 *
 * For local credential overrides (dev shortcut to bypass the captive
 * portal), copy include/secrets.example.h to include/secrets.h and fill
 * in the WIFI_DEFAULT_* / REST_DEFAULT_* macros there. secrets.h is
 * git-ignored.
 */
#pragma once

#include <stdint.h>

/* Pull in user-local overrides if they exist. Falls through silently if
 * secrets.h hasn't been created -- the build doesn't depend on it. */
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#  endif
#endif

/* ------------------------------------------------------------------ */
/* Panel + board hardware definition.                                 */
/* The pin map, panel geometry (EPD_WIDTH/HEIGHT/BUF_BYTES), palette,  */
/* MCU tier, and selected panel driver live in a per-board header      */
/* under boards/, chosen at build time by a -DTESSERAE_BOARD_* flag    */
/* (see boards/board.h). Every TU that includes app_config.h therefore */
/* still sees the EPD_* macros, exactly as before the multi-board      */
/* refactor when they lived directly here.                             */
/* ------------------------------------------------------------------ */
#include "board.h"

/* ------------------------------------------------------------------ */
/* Application behavior                                               */
/* ------------------------------------------------------------------ */

/* Reported in the status heartbeat so Tesserae can show which firmware
 * each device is running. The authoritative value is set in platformio.ini
 * (build_flags = -DFW_VERSION=\"x.y.z\"); this is just a fallback so the
 * file still compiles outside PlatformIO. */
#ifndef FW_VERSION
#define FW_VERSION         "0.0.0-dev"
#endif

/* Device "kind" reported to Tesserae at onboarding (discover/register). It
 * selects the server-side renderer: "esp32_client" packs a portrait 1200x1600
 * 4bpp Spectra-6 frame, which suits both the Waveshare 13.3E6 and the E1004.
 * A board with a different native geometry/colour mode can override this in its
 * board header before app_config.h pulls it in. */
#ifndef TESSERAE_DEVICE_KIND
#define TESSERAE_DEVICE_KIND  "esp32_client"
#endif

/* Human-readable board model, used to build the default device id
 * "<model>_<mac-suffix>" (e.g. reTerminal_E1004_859878). Each board header
 * sets its own; this is just a fallback so the file compiles standalone. */
#ifndef TESSERAE_DEVICE_MODEL
#define TESSERAE_DEVICE_MODEL  "esp32"
#endif

/* How long to deep-sleep between MQTT checks. 1 minute is the short
 * dev-friendly default; production cadence is normally pushed
 * server-side via the config topic (e.g. 15 min for a 6-colour panel
 * whose refresh itself takes ~30s) and persisted to NVS. */
#define SLEEP_INTERVAL_S   60

/* Cap on how long we'll wait for a retained MQTT message after
 * subscribing, before giving up and going back to sleep. */
#define MQTT_WAIT_MS       8000

/* WiFi STA connect attempt: retry count and per-attempt timeout. */
#define WIFI_CONNECT_RETRIES   5
#define WIFI_CONNECT_TIMEOUT_MS 15000

/* Resilience for an already-onboarded device: if WiFi fails on a wake (router
 * reboot, briefly out of range), don't nuke a working device into AP mode and
 * drain the battery. Sleep this long and retry; only after this many consecutive
 * failed wakes fall back to the captive portal. */
#define WIFI_RETRY_SLEEP_S       60
#define WIFI_FAIL_AP_THRESHOLD   12   /* ~12 min of retries before opening AP */

/* Max idle window the captive portal stays up with no client associated
 * to the SoftAP. The timer resets each time a STA joins the AP, so a user
 * actively filling in the form never times out. After this many seconds
 * with no client connected, the device deep-sleeps with no wakeup source
 * configured -- only a RESET / EXT button press boots it again. */
#define PROVISION_PORTAL_TIMEOUT_S  (15 * 60)

/* SoftAP credentials shown to the user during provisioning. */
#define PROVISION_AP_SSID    "Tesserae-Setup"
#define PROVISION_AP_PASS    "tesserae"     /* >= 8 chars or use open AP */

/* ------------------------------------------------------------------ */
/* WiFi / MQTT compile-time defaults                                  */
/* ------------------------------------------------------------------ */
/* Precedence on each wake:
 *     NVS (set via portal)  >  these defaults  >  empty (portal triggers)
 *
 * secrets.h may override any of these; otherwise WiFi defaults to empty
 * (no auto-connect) and MQTT defaults to placeholders that will fail
 * gracefully if the user hasn't run the portal yet. */

#ifndef WIFI_DEFAULT_SSID
#define WIFI_DEFAULT_SSID   ""
#endif
#ifndef WIFI_DEFAULT_PASS
#define WIFI_DEFAULT_PASS   ""
#endif

/* REST transport: the device registers against this Tesserae server URL over
 * <server_url>/api/v1/device/. Normally set via the captive portal; a dev
 * default can be provided in secrets.h. An optional REST_DEFAULT_PAIRING_CODE
 * opts into admin-gated register instead of zero-touch discover. */
#ifndef REST_DEFAULT_SERVER_URL
#define REST_DEFAULT_SERVER_URL   ""
#endif

/* Dev shortcut: define DEV_DISABLE_SLEEP (in secrets.h) to swap the
 * 15-min deep sleep for a short delay + software restart loop. Useful
 * while iterating with the serial monitor open. Cold-boot splash only
 * fires on power-on / RESET button, not on the software restart, so
 * each iteration is fast. */
#ifndef DEV_LOOP_INTERVAL_S
#define DEV_LOOP_INTERVAL_S 10
#endif

/* NVS namespaces / keys. WiFi creds live in "wifi"; REST transport config
 * (server URL, device token, pairing code, device id, frame ETag, sleep_s)
 * lives in "rest" and is owned by rest_config.[ch]. */
#define NVS_NS_WIFI        "wifi"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "pass"

/* Sanity bounds on the deep-sleep interval. The lower bound stops the server
 * accidentally turning the device into a spinner; the upper bound is just
 * "this is probably a bug". */
#define SLEEP_INTERVAL_MIN_S  30
#define SLEEP_INTERVAL_MAX_S  (7 * 24 * 60 * 60)
