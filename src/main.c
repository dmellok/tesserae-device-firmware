/*
 * tesserae-device-esp32-bin - battery-powered MQTT-driven e-paper client
 * for the Tesserae composer/renderer pipeline. Subscribes to a retained
 * frame URL, downloads the panel-native 4bpp .bin, paints it, and goes
 * back to deep sleep.
 *
 * Lifecycle of one wake:
 *
 *   boot
 *     -> wifi creds in NVS?           no  -> captive portal -> reboot
 *     -> connect STA                  fail-> captive portal -> reboot
 *     -> grab retained MQTT job       miss-> sleep (nothing new to show)
 *     -> url unchanged since last?    yes -> sleep (skip refresh)
 *     -> fetch + decode + paint panel fail-> sleep (try again next wake)
 *     -> persist new hash             -> deep sleep for SLEEP_INTERVAL_S
 */

#include <string.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "nvs_flash.h"      /* factory-reset erase (20 s refresh-button hold) */
#include "buttons.h"        /* front-button wake/report (header-only; no-op if none) */
#include "deck_run.h"       /* SD deck cache: local nav + sync (no-op w/o card) */
#include "deck_cache.h"     /* + sdcard.h/deck.h: DECK_SD_SELFTEST round trip */
#include "sdcard.h"
#include "esp_heap_caps.h"
#include "touch_gt911.h"    /* GT911 touch wake (guarded by BOARD_HAS_TOUCH) */
#include "touch_queue.h"    /* RTC replay queue for unsent touches (guarded) */
#include "touch_wakestub.h" /* RTC wake-stub early touch capture (guarded) */
#include "battery.h"
#include "epd_driver.h"
#include "image_decoder.h"
#include "image_fetcher.h"
#include "net_rest.h"
#include "ota_boot.h"
#include "ota_install.h"
#include "ota_manifest.h"
#include "ota_report.h"
#include "ota_verify.h"
#include "provisioning.h"
#include "rest_config.h"
#include "splash.h"
#include "wifi_manager.h"

static const char *TAG = "main";

/* ---------- double-tap-reset -> settings mode ---------- */

/* The tap counter lives in RTC slow memory so it survives the RESET button
 * (and deep sleep). A magic word tells "retained" apart from power-on garbage.
 *
 * NOTE: whether the RESET button preserves RTC memory is board-specific. If a
 * given board fully power-cycles the RTC domain on reset, the counter never
 * reaches 2 and double-tap simply never fires -- a harmless no-op. In that
 * case use the captive portal (no creds) or `idf.py erase-flash` instead. */
#define RTC_TAP_MAGIC  0x54455353u   /* 'TESS' */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static uint32_t s_reset_taps;

/* Consecutive failed WiFi connects across deep-sleep wakes (RTC-retained). An
 * already-onboarded device retries a few wakes on a transient outage before it
 * gives up and reopens the portal, instead of dropping to AP on the first miss. */
RTC_NOINIT_ATTR static uint32_t s_wifi_fail_count;

/* Monotonic id bumped on every button wake (RTC-retained) so the server can
 * dedup a retried request to one action; survives deep sleep, distinct per press. */
RTC_NOINIT_ATTR static uint32_t s_button_event_seq;

/* One-shot deep-sleep interval override (seconds); 0 = use the server interval.
 * Set before sleep to schedule a shorter WiFi retry backoff. */
static int32_t s_sleep_override_s = 0;

/* Increment on each manual reset; two within one wake window => settings mode.
 * The window is closed by zeroing the counter when we commit to deep sleep
 * (see sleep_forever_or_until_timer), so single taps minutes apart don't add
 * up to a false double-tap. */
static bool detect_settings_mode(esp_reset_reason_t reason)
{
    if (s_rtc_magic != RTC_TAP_MAGIC) {   /* power-on / garbage: seed it */
        s_rtc_magic = RTC_TAP_MAGIC;
        s_reset_taps = 0;
        s_wifi_fail_count = 0;
        s_button_event_seq = 0;
    }

    bool manual = (reason == ESP_RST_POWERON || reason == ESP_RST_EXT);
    if (manual) {
        s_reset_taps++;
    } else {
        s_reset_taps = 0;   /* timer wake / software restart isn't a tap */
    }

    if (s_reset_taps >= 2) {
        s_reset_taps = 0;
        return true;
    }
    return false;
}

/* ---------- REST cycle helpers ---------- */

/* Resolve a (possibly relative) frame URL against the server origin. Ported
 * from the pico-bin client -- the /frame endpoint may return a path-only url. */
static void resolve_url(const char *server, const char *u, char *out, size_t cap)
{
    if (strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0) {
        snprintf(out, cap, "%s", u);
        return;
    }
    char origin[200];
    snprintf(origin, sizeof origin, "%s", server);
    char *p = strstr(origin, "://");
    p = p ? p + 3 : origin;
    char *sl = strchr(p, '/');
    if (sl) *sl = '\0';                      /* drop any path on the server_url */
    snprintf(out, cap, "%s%s%s", origin, (u[0] == '/') ? "" : "/", u);
}

static int current_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

/* Effective deep-sleep interval: the REST-config value (server-driven via
 * next_poll_s / sleep_interval_s), clamped to sane bounds. */
static int effective_sleep_s(void)
{
    int32_t v = rest_config_get()->sleep_s;
    if (v < SLEEP_INTERVAL_MIN_S || v > SLEEP_INTERVAL_MAX_S) return SLEEP_INTERVAL_S;
    return v;
}

/* ---------- deep sleep ---------- */

static void sleep_forever_or_until_timer(void)
{
    /* Card off before every deep sleep (and before a dev-loop restart);
     * no-op when nothing is mounted. */
    deck_pre_sleep();

    /* Decide between deep sleep (battery) and short-delay restart loop (dev):
     *   DEV_DISABLE_SLEEP defined -> always loop (dev override)
     *   DEV_FORCE_SLEEP   defined -> always deep-sleep, even on USB host
     *                                (exercise battery path while plugged in)
     *   otherwise -> auto-detect: USB host (laptop / SOF-emitter) loops, a
     *                bare USB charger / power bank does not emit SOFs and is
     *                treated as battery. */
    /* Close the double-tap window: once we're committing to sleep/loop, a
     * later single reset should start counting from zero again. */
    s_reset_taps = 0;

#if defined(DEV_DISABLE_SLEEP) && defined(DEV_FORCE_SLEEP)
#  error "DEV_DISABLE_SLEEP and DEV_FORCE_SLEEP are mutually exclusive"
#endif

    bool loop = false;
    const char *reason = NULL;

#ifdef DEV_DISABLE_SLEEP
    loop = true;
    reason = "DEV_DISABLE_SLEEP";
#elif defined(DEV_FORCE_SLEEP)
    /* Skip the USB-host check entirely; behave as if on battery. */
#else
    if (usb_serial_jtag_is_connected()) {
        loop = true;
        reason = "USB host detected";
    }
#endif

    if (loop) {
        ESP_LOGI(TAG, "%s: software restart in %d s", reason, DEV_LOOP_INTERVAL_S);
        vTaskDelay(pdMS_TO_TICKS(DEV_LOOP_INTERVAL_S * 1000));
        esp_restart();
    }

    int interval = (s_sleep_override_s > 0) ? s_sleep_override_s : effective_sleep_s();
    ESP_LOGI(TAG, "on battery; deep sleep for %d s%s",
             interval,
             s_sleep_override_s > 0 ? " (retry backoff)"
               : (interval == SLEEP_INTERVAL_S) ? " (default)" : " (server-driven)");
    /* epd_sleep() already dropped the panel power rail; no extra cleanup
     * needed before going down. */
    /* Wake on any front button too (armed as one ext1 mask; no-op if the board
     * has none). A press wakes early and, via the button report, drives the
     * server action (refresh / rotate) on the next boot. See buttons.h. */
#if BOARD_HAS_TOUCH
    /* Fold the active-low touch INT into the button ext1 ANY_LOW mask when the
     * server enabled touch. touch_prepare_sleep() leaves the GT911 scanning and
     * latches TP_RST across sleep so the controller keeps its address. Off by
     * default -> touch_wake_mask stays 0 and this is just the buttons. */
    uint64_t touch_wake_mask = 0;
    if (rest_config_get()->touch_enabled) {
        touch_prepare_sleep();
        touch_wake_mask = TOUCH_INT_WAKE_MASK;
    }
    buttons_arm_ext1_with(touch_wake_mask);
#else
    buttons_arm_ext1();
#endif
    esp_sleep_enable_timer_wakeup((uint64_t)interval * 1000000ULL);
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- app ---------- */

static void run_provisioning_then_reboot(const char *note)
{
    ESP_LOGW(TAG, "opening captive portal%s%s", note ? ": " : "", note ? note : "");
    /* Bring the AP up FIRST (joinable in ~1-2 s), THEN paint the portal splash.
     * The splash is a ~25-30 s blocking panel refresh (worst on the 13.3"), so
     * doing it first would leave the AP dark that whole time -- and the QR it
     * shows would point at an AP that isn't up yet. With begin() first, the AP
     * is live while the panel renders, and a submit during the paint is captured
     * on the httpd task (serve() picks it up). `note` (when set) replaces the
     * "Setup mode" subtitle with why the user was sent back here. */
    provisioning_begin();
    if (note) splash_show_portal_note(note);
    else      splash_show_portal();
    esp_err_t err = provisioning_serve();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "creds saved; rebooting to use them");
        esp_restart();
        /* not reached */
    }

    /* Portal expired with no client ever joining (or no submission). Don't
     * spin the radio retrying every N minutes -- power down completely and
     * require a manual RESET to come back. The board's RESET button (chip
     * EN line) causes a fresh boot which re-enters the portal. */
    ESP_LOGW(TAG, "captive portal expired idle; deep sleep until RESET button");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
    /* not reached */
}

/* Show the splash on a true cold boot -- power-on or RESET button. Skip
 * it on timer-wake (production sleep cycle) AND on software restart
 * (DEV_DISABLE_SLEEP / DEV_FORCE_SLEEP loop iterations), so we don't burn
 * 25-30 s of panel refresh on every quick test cycle.
 *
 * If we'll be going straight to the captive portal anyway (no creds), skip
 * the logo splash -- run_provisioning_then_reboot() paints the portal splash
 * instead, avoiding a wasted second ~30 s refresh on the no-creds path. */
static void maybe_show_splash(esp_reset_reason_t reset_reason, bool has_creds)
{
    if (reset_reason != ESP_RST_POWERON && reset_reason != ESP_RST_EXT) {
        return;
    }
    if (!has_creds) {
        return;
    }
    ESP_LOGI(TAG, "cold boot; showing logo splash");
    splash_show_logo();
}

/* ---------- REST onboarding ---------- */

/* Sanity window for the wall clock (set from each REST response's Date header):
 * now must look like a real time before we ship a sleep_until in the status. */
#define EPOCH_REASONABLE_MIN 1700000000LL   /* 2023-11-14 */
#define EPOCH_REASONABLE_MAX 2200000000LL   /* 2039-09-13 */

/* Onboarding outcome, so the caller can give the right on-screen feedback. */
typedef enum {
    BOOTSTRAP_OK,           /* hold a token now -- continue the cycle          */
    BOOTSTRAP_PENDING,      /* server reachable, awaiting admin approval        */
    BOOTSTRAP_UNREACHABLE,  /* can't reach Tesserae / code rejected -> portal   */
} bootstrap_res_t;

/* Persisted onboarding-splash state (rest_config_get/set_ui_state), so a status
 * splash repaints only on a real transition -- not on every retry wake or, on
 * USB dev power, every software-restart loop while the state is unchanged. */
enum { UI_NONE = 0, UI_PENDING = 1, UI_CONNECTED = 2 };

/* First-boot onboarding: obtain a device token. On a non-OK result a backoff is
 * written to rest_config's sleep_s and a short human reason into `note` (for the
 * portal subtitle on UNREACHABLE, or the message body on PENDING). Zero-touch
 * discover/claim by default (admin clicks Register in Tesserae, no typing on the
 * device); a stored pairing code opts into strict admin-gated register. Ported
 * from tesserae-device-pico-bin main.c rest_bootstrap(). */
static bootstrap_res_t rest_bootstrap(uint16_t pw, uint16_t ph, const char *mac,
                                      bool *dirty, char *note, size_t note_sz)
{
    const rest_config_t *c = rest_config_get();
    if (c->device_token[0] != '\0') return BOOTSTRAP_OK;   /* already bootstrapped */

    if (c->pairing_code[0] != '\0') {
        rest_register_out_t ro;
        rest_status_t rs = rest_register(pw, ph, mac, FW_VERSION, &ro, 10000);
        if (rs == REST_OK) {
            rest_config_set_device_token(ro.token);
            /* Adopt the server's canonical device id (MAC-matched; the token is
             * bound to it -- frame/status URLs must use it or the server 403s). */
            if (ro.device_id[0] && strcmp(ro.device_id, rest_config_device_id()) != 0)
                rest_config_set_device_id(ro.device_id);
            rest_config_set_pairing("");            /* one-shot; clear on success */
            if (ro.sleep_interval_s > 0) rest_config_set_sleep_s(ro.sleep_interval_s);
            if (ro.button_wake_s >= 0) rest_config_set_button_wake_s(ro.button_wake_s);
            *dirty = true;
            ESP_LOGI(TAG, "registered via pairing code; token stored (id=%s)",
                     rest_config_device_id());
            return BOOTSTRAP_OK;
        }
        int32_t backoff = (c->sleep_s > 0) ? c->sleep_s : 900;
        bootstrap_res_t res = BOOTSTRAP_UNREACHABLE;
        if (rs == REST_UNAUTH || rs == REST_FORBIDDEN) {
            backoff = 3600;
            ESP_LOGW(TAG, "register rejected (%d); sleeping 1h to re-pair", rs);
            snprintf(note, note_sz, "Pairing code rejected");   /* -> portal */
        } else if (rs == REST_RATELIMIT) {
            backoff = (ro.retry_after_s > 0) ? ro.retry_after_s : 3600;
            ESP_LOGW(TAG, "register rate-limited; backoff %lds", (long)backoff);
            snprintf(note, note_sz, "Tesserae is busy; it will keep retrying.");
            res = BOOTSTRAP_PENDING;
        } else {
            ESP_LOGW(TAG, "register failed (%d); retry next cycle", rs);
            snprintf(note, note_sz, "Can't reach the server");   /* -> portal */
        }
        rest_config_set_sleep_s(backoff);
        return res;
    }

    /* Zero-touch: announce via discover and claim the token once the admin
     * clicks Register. Retried every wake, by design (no caching). */
    rest_discover_out_t dd;
    rest_status_t ds = rest_discover(pw, ph, mac, FW_VERSION, &dd, 10000);
    if (ds == REST_OK && dd.registered) {
        rest_config_set_device_token(dd.token);
        if (dd.device_id[0] && strcmp(dd.device_id, rest_config_device_id()) != 0) {
            ESP_LOGI(TAG, "adopting server device id '%s' (was '%s')",
                     dd.device_id, rest_config_device_id());
            rest_config_set_device_id(dd.device_id);
        }
        if (dd.sleep_interval_s > 0) rest_config_set_sleep_s(dd.sleep_interval_s);
        *dirty = true;
        ESP_LOGI(TAG, "claimed token via discover; bootstrap complete (id=%s)",
                 rest_config_device_id());
        return BOOTSTRAP_OK;
    }
    int32_t backoff;
    bootstrap_res_t res;
    if (ds == REST_OK) {            /* registered:false, awaiting admin Register */
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 30;
        ESP_LOGI(TAG, "discovered, waiting for admin to Register; retry in %lds",
                 (long)backoff);
        snprintf(note, note_sz, "Approve this device in Tesserae, Settings > Devices.");
        res = BOOTSTRAP_PENDING;
    } else if (ds == REST_RATELIMIT) {
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 60;
        ESP_LOGW(TAG, "discover rate-limited; backoff %lds", (long)backoff);
        snprintf(note, note_sz, "Tesserae is busy; it will keep retrying.");
        res = BOOTSTRAP_PENDING;
    } else {
        backoff = 30;   /* unreachable (e.g. wrong server URL): send to portal */
        ESP_LOGW(TAG, "discover failed (%d); retry in %lds", ds, (long)backoff);
        snprintf(note, note_sz, "Can't reach the server");
        res = BOOTSTRAP_UNREACHABLE;
    }
    rest_config_set_sleep_s(backoff);
    return res;
}

#if BOARD_HAS_TOUCH || defined(BOARD_HAS_BUTTONS)
/* Fetch the current frame and paint it if the server returned a new one. Used
 * inside the touch-linger and button-wake windows, where WiFi is kept up so
 * repeated interactions don't pay reconnect/boot latency. The touch/button
 * params must already be set on the REST client. Returns true if a new frame
 * was painted. */
static bool fetch_and_paint_current(const char *server_url)
{
    rest_frame_out_t fo;
    if (rest_get_frame(&fo, 8000) != REST_OK) return false;   /* 304/204/err: nothing new */
    /* A 200 body carries the freshest button_wake_s; adopt it mid-window too. */
    if (fo.button_wake_s >= 0) rest_config_set_button_wake_s(fo.button_wake_s);
    char fullurl[512];
    resolve_url(server_url, fo.url, fullurl, sizeof fullurl);
    fetched_image_t img;
    if (image_fetch(fullurl, &img) != ESP_OK) return false;
    uint8_t *frame = NULL;
    if (image_decode_to_frame(&img, fullurl, &frame) != ESP_OK) frame = NULL;
    free(img.data);
    if (!frame) return false;
    ESP_ERROR_CHECK(epd_port_init());
    epd_init();
    epd_display(frame);
    epd_sleep();
    free(frame);
    if (fo.etag[0]) rest_config_set_frame_etag(fo.etag);
    return true;
}
#endif /* BOARD_HAS_TOUCH || BOARD_HAS_BUTTONS */

#if BOARD_HAS_TOUCH
/* Replay strokes queued from earlier wakes whose WiFi connect had failed. Each
 * is dispatched via a frame GET (the response is not painted -- the current
 * cycle paints the live frame); a completed request pops it, a transient network
 * error keeps it for next time. Call while WiFi is up. */
static void touch_queue_flush(void)
{
    touch_qentry_t e;
    int guard = 0;
    while (touch_queue_front(&e) && guard++ < TOUCH_QUEUE_MAX) {
        rest_set_touch(e.x0, e.y0, e.x1, e.y1, e.ms, e.digest, e.event_id);
        rest_frame_out_t fo;
        rest_status_t fs = rest_get_frame(&fo, 8000);
        if (fs == REST_NET_ERR || fs == REST_RATELIMIT) break;   /* transient: retry next wake */
        ESP_LOGI(TAG, "replayed queued touch (event %u) -> %d", (unsigned)e.event_id, fs);
        touch_queue_pop();   /* dispatched, stale-dropped, or unrecoverable -> remove */
    }
    rest_set_touch(0, 0, 0, 0, 0, NULL, 0);   /* clear so it doesn't leak into later GETs */
}
#endif

#if TESSERAE_OTA_CAPABILITY_ENABLED
/* Map a non-applied install result onto the contract's reason vocabulary
 * (docs/ota/contract.md "Reason codes"). The apply-time battery gate is a
 * `rejected` per the contract; transport errors fold into download_error;
 * slot/argument problems into flash_error. */
static void ota_report_install_failure(const ota_manifest_t *m,
                                       ota_install_result_t r,
                                       const char *attempt)
{
    switch (r) {
    case OTA_INSTALL_LOW_BATTERY:
        ota_report_set(OTA_REPORT_REJECTED, "battery_low", m->fw_version, attempt);
        break;
    case OTA_INSTALL_HTTP_INIT_FAILED:
    case OTA_INSTALL_HTTP_FAILED:
    case OTA_INSTALL_HTTP_STATUS:
        ota_report_set(OTA_REPORT_FAILED, "download_error", m->fw_version, attempt);
        break;
    case OTA_INSTALL_SIZE_MISMATCH:
        ota_report_set(OTA_REPORT_FAILED, "size_mismatch", m->fw_version, attempt);
        break;
    case OTA_INSTALL_DIGEST_MISMATCH:
        ota_report_set(OTA_REPORT_FAILED, "digest_mismatch", m->fw_version, attempt);
        break;
    case OTA_INSTALL_IMAGE_INVALID:
        ota_report_set(OTA_REPORT_FAILED, "image_invalid", m->fw_version, attempt);
        break;
    default:
        ota_report_set(OTA_REPORT_FAILED, "flash_error", m->fw_version, attempt);
        break;
    }
}
#endif

/* ---------- low-battery goodbye ---------- */

/* Goodbye-on-screen flag lives in RTC RAM so it survives the hibernate sleeps;
 * a power loss or RESET repaints the goodbye once if the cell is still flat. */
RTC_DATA_ATTR static uint8_t s_goodbye_on_screen;

/* Set when a wake finds the battery recovered while the goodbye is still on
 * the panel: the cycle must drop the cached ETag so the server sends a full
 * frame (a 304 would leave the goodbye up forever). */
static bool s_battery_recovered;

/* Battery-empty gate, run before any radio work. Returns only when the cell is
 * healthy, absent, or unknown. On a flat cell: paint the goodbye once, then
 * hibernate on an hourly recheck (front buttons still wake early, so plugging
 * in a charger and pressing a key resumes without waiting out the hour). */
static void battery_goodbye_check(bool settings_mode)
{
    if (settings_mode) return;   /* never lock the operator out of settings */

    int mv = battery_read_mv();
    if (mv < BATTERY_PRESENT_MIN_MV) return;   /* no/unknown cell (0 on mains) */

    if (mv >= (s_goodbye_on_screen ? BATTERY_RESUME_MV : BATTERY_GOODBYE_MV)) {
        if (s_goodbye_on_screen) {
            ESP_LOGW(TAG, "battery recovered (%d mV); resuming with a fresh frame", mv);
            s_goodbye_on_screen = 0;
            s_battery_recovered = true;
        }
        return;
    }

    if (!s_goodbye_on_screen) {
        ESP_LOGW(TAG, "battery empty (%d mV); painting goodbye and hibernating", mv);
        splash_show_message("Battery empty", "Recharge me to wake this display");
        s_goodbye_on_screen = 1;
    } else {
        ESP_LOGW(TAG, "battery still empty (%d mV); back to hibernation", mv);
    }
    buttons_arm_ext1();
    esp_sleep_enable_timer_wakeup((uint64_t)BATTERY_GOODBYE_RECHECK_S * 1000000ULL);
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- front-button factory reset ---------- */

/* Hold the refresh button (Key1) for FACTORY_RESET_HOLD_S to erase NVS and
 * reboot into the setup portal. The initiating press is an ordinary ext1 wake
 * (or the button can be held through a RESET/power-on), so this runs on every
 * boot but costs one GPIO read when the button is already up. It must run
 * BEFORE wifi_manager_init() so the erase cannot race the connect path's own
 * NVS writes (fast-connect hints, creds). Held through the reboot, the check
 * simply re-arms: NVS is already blank, and the portal opens on release. */
#if defined(BOARD_HAS_BUTTONS) && defined(BOARD_BTN_REFRESH_PIN)
static void maybe_factory_reset_hold(button_id_t woke_btn, bool first_boot)
{
    if (woke_btn != BTN_REFRESH && !first_boot) return;

    buttons_poll_init();
    vTaskDelay(pdMS_TO_TICKS(10));   /* pull-up settle before the first read */

    int held_ms = 0;
    while (gpio_get_level((gpio_num_t)BOARD_BTN_REFRESH_PIN) == 0) {
        if (held_ms == 0) {
            ESP_LOGW(TAG, "refresh button held: factory reset in %d s (release to cancel)",
                     FACTORY_RESET_HOLD_S);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        held_ms += 100;
        if (held_ms % 5000 == 0 && held_ms < FACTORY_RESET_HOLD_S * 1000) {
            ESP_LOGW(TAG, "still holding: factory reset in %d s",
                     FACTORY_RESET_HOLD_S - held_ms / 1000);
        }
        if (held_ms >= FACTORY_RESET_HOLD_S * 1000) {
            ESP_LOGW(TAG, "factory reset: erasing NVS (creds + config) and rebooting into setup");
            nvs_flash_erase();   /* NVS not yet initialised here, so this is legal */
            esp_restart();
            /* not reached */
        }
    }
    if (held_ms) {
        ESP_LOGI(TAG, "released after %d ms; continuing as a normal press", held_ms);
    }
}
#else
static void maybe_factory_reset_hold(button_id_t woke_btn, bool first_boot)
{
    (void)woke_btn; (void)first_boot;
}
#endif

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool settings_mode = detect_settings_mode(reset_reason);
    /* A "first boot" (power-on, RESET button, or the reboot right after a portal
     * save) vs a timer wake from deep sleep. Connect-feedback splashes that we
     * only want to show once (not on every 30 s retry wake) key off this. */
    bool first_boot = (reset_reason != ESP_RST_DEEPSLEEP);
    ESP_LOGI(TAG, "boot; reset_reason=%d wakeup_cause=%d settings_mode=%d first_boot=%d",
             reset_reason, esp_sleep_get_wakeup_cause(), settings_mode, first_boot);

    /* Front-button wake (see buttons.h): a press wakes us early via ext1. We tell
     * the REST client which button so the frame/status requests carry it, and we
     * force a fresh paint this cycle (drop the cached ETag below -> server returns
     * 200, not 304). Server maps refresh/left/right -> refresh/rotate_prev/next. */
    button_id_t woke_btn = buttons_which_woke();
    bool woke_by_button = (woke_btn != BTN_NONE);
    if (woke_by_button) {
        uint32_t ev = ++s_button_event_seq;
        ESP_LOGI(TAG, "woke on '%s' button: report + refresh (event %u)",
                 button_name(woke_btn), (unsigned)ev);
        rest_set_button(button_name(woke_btn), ev);
    }

#ifdef BATTERY_DEBUG_SWEEP
    /* Battery sense bring-up: log every ADC1 channel to find the real sense pin
     * + divider. No networking. Enable with -DBATTERY_DEBUG_SWEEP. */
    ESP_LOGW(TAG, "BATTERY_DEBUG_SWEEP: logging ADC channels (no networking)");
    battery_debug_sweep();   /* never returns */
#endif

#ifdef EPD_SELFTEST
    /* Panel-driver bring-up: no WiFi, no MQTT. Init the active panel and paint
     * the 6 colour bars, then sleep and halt. Use this to validate a new/ported
     * driver (pins, init sequence, refresh) in isolation. If every band shows
     * the expected ink, the driver + panel + LUT are healthy. Enable with
     * -DEPD_SELFTEST in the env's build_flags. */
    ESP_LOGW(TAG, "EPD_SELFTEST: painting colour bars (no networking)");
    ESP_ERROR_CHECK(epd_port_init());
    epd_init();
    epd_show_color_bars();
    epd_sleep();
#if BOARD_HAS_TOUCH
    /* Touch wiring check: probe the GT911 and stream raw + frame-translated
     * coordinates on every touch, so the digitiser and the orientation flags can
     * be verified with no server. A panel-corner tap should print frame (0,0). */
    if (touch_init() == ESP_OK) {
        ESP_LOGW(TAG, "EPD_SELFTEST: GT911 id=0x%08x; touch the panel (raw -> frame)",
                 (unsigned)touch_product_id());
        while (1) {
            int rx = 0, ry = 0, fx = 0, fy = 0; bool pressed = false;
            if (touch_read_raw(&rx, &ry, &pressed) == ESP_OK && pressed) {
                touch_translate_raw(rx, ry, &fx, &fy);   /* same point, no re-read */
                ESP_LOGI(TAG, "touch raw=(%d,%d) -> frame=(%d,%d)", rx, ry, fx, fy);
            }
            vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
        }
    }
    ESP_LOGW(TAG, "EPD_SELFTEST: GT911 not found; halting. Press RESET to repeat.");
#else
    ESP_LOGW(TAG, "EPD_SELFTEST: done; halting. Press RESET to repeat.");
#endif
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

#ifdef DECK_SD_SELFTEST
    /* Deck-cache bring-up: mount the card, run one frame-sized write ->
     * read-back -> digest-verify round trip through the REAL cache path
     * (tmp+rename, sha256 gate), report free space, halt. No networking, no
     * panel refresh. Enable with -DDECK_SD_SELFTEST (see the -sdtest env). */
    ESP_LOGW(TAG, "DECK_SD_SELFTEST: mount + write/read/verify (no networking)");
    if (!sdcard_mount()) {
        ESP_LOGE(TAG, "DECK_SD_SELFTEST: mount FAILED (no card / bad card / wiring)");
    } else {
        ESP_LOGW(TAG, "DECK_SD_SELFTEST: mounted, %llu MB free",
                 (unsigned long long)(sdcard_free_bytes() >> 20));
        size_t n = EPD_BUF_BYTES;
        uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = malloc(n);
        if (!buf) {
            ESP_LOGE(TAG, "DECK_SD_SELFTEST: OOM for %u-byte test frame", (unsigned)n);
        } else {
            for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 31 + (i >> 8));
            uint8_t sha[32];
            char digest[DECK_DIGEST_HEX + 1];
            deck_sha256(buf, n, sha);
            deck_digest_hex16(sha, digest);
            bool w = deck_cache_write_frame("sdtest", digest, buf, n, (uint32_t)n);
            ESP_LOGW(TAG, "DECK_SD_SELFTEST: write+verify %s (%u bytes, digest %s)",
                     w ? "OK" : "FAILED", (unsigned)n, digest);
            uint8_t *back = NULL;
            bool r = w && deck_cache_read_frame("sdtest", digest, (uint32_t)n, &back);
            bool same = r && memcmp(buf, back, n) == 0;
            ESP_LOGW(TAG, "DECK_SD_SELFTEST: read-back %s, contents %s",
                     r ? "OK" : "FAILED", same ? "MATCH" : "MISMATCH");
            free(back);
            free(buf);
            deck_cache_delete("sdtest", digest);
            ESP_LOGW(TAG, "DECK_SD_SELFTEST: %s. Press RESET to repeat.",
                     (w && r && same) ? "ALL CHECKS PASSED" : "FAILED");
        }
        sdcard_unmount();
    }
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

    /* Battery-empty gate first (a flat cell should not pay for radio bring-up
     * or a portal), then the factory-reset hold -- both before any NVS/WiFi
     * init so the erase path is race-free. */
    battery_goodbye_check(settings_mode);
    maybe_factory_reset_hold(woke_btn, first_boot);

    ESP_ERROR_CHECK(wifi_manager_init());
    rest_config_load();

    /* Recovered from the battery goodbye this wake: the goodbye is still on
     * the panel, so drop the cached ETag -- the server must send a full frame
     * (a 304 would leave the goodbye up). */
    if (s_battery_recovered) rest_config_set_frame_etag("");

    /* Deck cache boot: probe the SD card, advertise the capability, restore
     * nav state. Everything degrades to a no-op without a card. */
    deck_boot();

    /* Local deck nav: a button wake whose press matches a cached link on the
     * current page paints from SD (1-2 s) and never brings the radio up. Any
     * miss keeps today's exact network behaviour -- the press's report is
     * still pending from above. */
    if (woke_by_button) {
        bool nav_fallthrough = false;
        if (deck_try_button(button_name(woke_btn), &s_button_event_seq,
                            &nav_fallthrough)) {
            if (!nav_fallthrough) {
                ESP_LOGI(TAG, "served locally from deck cache; back to sleep");
                sleep_forever_or_until_timer();
                return;   /* not reached */
            }
            /* A press during the local window had no cached link: its report
             * is armed; run the normal network cycle for it. */
        }
    }

    /* Stay awake after the paint for further interaction? Set at the radio-down
     * decision: a touch wake with touch_linger_s, or a button wake with
     * button_wake_s (issue #123). Always false for timer/scheduled wakes. */
    bool will_linger = false; (void)will_linger;

#if BOARD_HAS_TOUCH
    /* Touch wake (GT911, EXT0). Capture the stroke ASAP -- before the multi-second
     * WiFi connect -- while the finger may still be down, then dispatch it on the
     * frame GET exactly like a button wake (server classifies + repaints). A wake
     * is button XOR touch (different wake causes). Off unless the server enabled
     * touch_enabled; the wake source was armed on last sleep from that config. */
    /* Touch shares the button ext1 ANY_LOW mask (TP_INT is active-low); a touch
     * wake is an ext1 wake whose status latch shows the TP_INT bit and no button
     * bit (so buttons_which_woke() above returned BTN_NONE for it). */
    bool woke_by_touch = rest_config_get()->touch_enabled &&
                         esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
                         (esp_sleep_get_ext1_wakeup_status() & TOUCH_INT_WAKE_MASK);
    /* Kept in function scope so the WiFi-fail path can queue an unsent stroke. */
    touch_stroke_t touch_st = { .valid = false };
    uint32_t       touch_ev = 0;
    if (woke_by_touch) {
        if (touch_init() == ESP_OK) {
            /* The RTC wake stub may have grabbed the point ~1 ms after wake. Take
             * it now (unconditionally, so it can never replay on a later wake) and
             * fall back to it only if the live read races an already-lifted finger. */
            int  stub_rx = 0, stub_ry = 0;
            bool have_stub = touch_wakestub_take(&stub_rx, &stub_ry);
#ifdef BOARD_TOUCH_WAKE_STUB
            ESP_LOGI(TAG, "wake stub: runs=%u stage=%u status=0x%02x captured=%d",
                     (unsigned)g_touch_wake_capture.runs,
                     (unsigned)g_touch_wake_capture.stage,
                     (unsigned)g_touch_wake_capture.status, (int)have_stub);
#endif

            touch_capture_stroke(&touch_st, TOUCH_FIRST_POINT_MS, TOUCH_CAP_MS);
            if (!touch_st.valid && have_stub) {
                /* Quick tap: finger gone before the ~1 s boot let us read a live
                 * point, but the stub caught it. Dispatch it as a zero-length tap. */
                int fx = 0, fy = 0;
                touch_translate_raw(stub_rx, stub_ry, &fx, &fy);
                touch_st.x0 = touch_st.x1 = fx;
                touch_st.y0 = touch_st.y1 = fy;
                touch_st.ms = 0;
                touch_st.valid = true;
                ESP_LOGI(TAG, "touch recovered from wake stub: raw (%d,%d) -> frame (%d,%d)",
                         stub_rx, stub_ry, fx, fy);
            }
            if (touch_st.valid) {
                touch_ev = ++s_button_event_seq;   /* shares the wake-event counter */
                ESP_LOGI(TAG, "touch (%d,%d)->(%d,%d) %ums (event %u)",
                         touch_st.x0, touch_st.y0, touch_st.x1, touch_st.y1,
                         (unsigned)touch_st.ms, (unsigned)touch_ev);
                /* Local deck nav: a tap landing in a cached link zone on the
                 * current page paints from SD and never reaches the server
                 * (the release point decides the hit). Misses fall through to
                 * today's dispatch below. */
                if (deck_try_touch(touch_st.x1, touch_st.y1)) {
                    ESP_LOGI(TAG, "touch served locally from deck cache; back to sleep");
                    sleep_forever_or_until_timer();
                    return;   /* not reached */
                }
                rest_set_touch(touch_st.x0, touch_st.y0, touch_st.x1, touch_st.y1,
                               touch_st.ms, rest_config_get()->last_frame_etag, touch_ev);
            } else {
                /* Quick-tap race: the finger lifted before the ~1 s deep-sleep
                 * boot let us read a point. Do NOT force a repaint -- fall through
                 * to a normal poll (keeps If-None-Match, so an unchanged frame
                 * 304s and the slow panel is not needlessly redrawn). A press is
                 * only dispatched when we actually capture a coordinate; hold
                 * briefly, or use touch_linger for responsive follow-up taps. */
                ESP_LOGI(TAG, "touch wake, no point readable in %d ms; normal poll",
                         TOUCH_FIRST_POINT_MS);
            }
        } else {
            ESP_LOGW(TAG, "touch wake but GT911 init failed; normal poll");
        }
    }
#endif

    /* Skip the 30 s splash when entering settings mode -- the user is waiting
     * on the editor, not a panel sanity check. */
    bool have_creds = wifi_creds_present();
    if (!settings_mode) {
        maybe_show_splash(reset_reason, have_creds);
    }

    /* The captive portal collects both WiFi and the Tesserae server URL, so a
     * device missing either is not yet usable -- send it to provisioning. */
    if (!have_creds || !rest_config_has_server()) {
        if (have_creds) {
            ESP_LOGW(TAG, "wifi set but no Tesserae server URL; opening portal");
            run_provisioning_then_reboot("Add your Tesserae server URL");
        } else {
            run_provisioning_then_reboot(NULL);
        }
        return;
    }

    esp_err_t err = wifi_sta_connect_stored();
    if (err != ESP_OK) {
#if BOARD_HAS_TOUCH
        /* Couldn't send this wake's touch -- queue it (RTC) to replay once WiFi
         * is back. The displayed frame rarely changes on e-paper between wakes,
         * so the digest is usually still valid then. */
        if (touch_st.valid)
            touch_queue_push(&touch_st, touch_ev, rest_config_get()->last_frame_etag);
#endif
        /* A device that has already onboarded (holds a token) almost always
         * fails here because of a transient outage -- a router reboot or being
         * briefly out of range -- not bad credentials. Don't drop a working
         * device into AP mode on the first miss (that strands it off-network and
         * burns the battery on an always-on radio). Retry over a few short-sleep
         * wakes; only after WIFI_FAIL_AP_THRESHOLD consecutive misses reopen the
         * portal. A never-onboarded device goes straight to the portal, since
         * there the creds themselves are the likely problem. */
        bool onboarded = rest_config_get()->device_token[0] != '\0';
        if (onboarded && ++s_wifi_fail_count < WIFI_FAIL_AP_THRESHOLD) {
            ESP_LOGW(TAG, "STA connect failed (%s); onboarded, retry %lu/%d in %ds "
                     "(not opening AP)", esp_err_to_name(err),
                     (unsigned long)s_wifi_fail_count, WIFI_FAIL_AP_THRESHOLD,
                     WIFI_RETRY_SLEEP_S);
            wifi_sta_stop();
            s_sleep_override_s = WIFI_RETRY_SLEEP_S;
            sleep_forever_or_until_timer();
            return;
        }
        ESP_LOGE(TAG, "STA connect failed (%s)%s; opening portal",
                 esp_err_to_name(err),
                 onboarded ? " after repeated retries" : " (not onboarded)");
        s_wifi_fail_count = 0;
        run_provisioning_then_reboot("Wi-Fi didn't connect");
        return;
    }
    s_wifi_fail_count = 0;   /* connected -> clear the retry streak */
#if TESSERAE_OTA_CAPABILITY_ENABLED
    /* Confirm a first-boot image only after the agreed local checks, including
     * WiFi association, pass. Tesserae server reachability is intentionally not
     * part of the rollback gate. Then resolve a persisted pending_confirm into
     * confirmed (we are the target version) or rolled_back (the bootloader
     * reverted the slot), so the next heartbeat reports the terminal state. */
    ota_boot_confirm_if_pending();
    ota_report_resolve_boot(FW_VERSION);
#endif

    /* Double-tap reset: serve the always-on settings editor on the LAN instead
     * of running the paint cycle. Stays up until a save (then reboot) or the
     * portal timeout (then sleep). */
    if (settings_mode) {
        ESP_LOGI(TAG, "settings mode: serving LAN editor + mDNS");
        if (settings_server_run_blocking() == ESP_OK) {
            ESP_LOGI(TAG, "settings saved; rebooting to apply");
            esp_restart();
        }
        ESP_LOGI(TAG, "settings editor timed out; back to sleep");
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }

    /* ---- one Tesserae cycle over the REST API ----
     * Bootstrap a token if needed, GET the frame (ETag/304 dedup), download +
     * decode, POST status (which drives the next sleep via next_poll_s), then
     * paint with the radio down. The wall clock is set from each response's
     * HTTP Date header, so no SNTP round-trip is needed. Mirrors the pico-bin
     * do_cycle_rest ordering. */
    const rest_config_t *c = rest_config_get();
    const uint16_t pw = EPD_WIDTH, ph = EPD_HEIGHT;   /* portrait-native 1200x1600 */
    char mac[18];
    rest_config_mac(mac, sizeof mac);

    bool cfg_dirty      = false;
    bool skip_paint     = false;
    bool just_onboarded = false;
    uint8_t *frame  = NULL;
    char new_etag[80] = {0};
    bool deck_sync_needed = false;   /* status asked for a deck cache resync */
    char deck_srv_ver[DECK_VERSION_CAP] = {0};

    /* 1. Bootstrap a device token (discover/claim, or register with a code). */
    if (c->device_token[0] == '\0') {
        char note[80] = {0};
        bootstrap_res_t br = rest_bootstrap(pw, ph, mac, &cfg_dirty, note, sizeof note);
        if (br != BOOTSTRAP_OK) {
            /* No token yet; rest_config sleep_s holds the backoff. The common
             * "waiting for admin" retry is not persisted (avoid flash wear);
             * only persist if a code register/adopt changed something. */
            if (cfg_dirty) rest_config_save();
            wifi_sta_stop();
            if (br == BOOTSTRAP_UNREACHABLE) {
                /* Genuinely can't reach Tesserae (bad URL / server down / code
                 * rejected): keep the portal up with the reason so the user can
                 * fix it. Reboots on save; deep-sleeps if the portal times out. */
                run_provisioning_then_reboot(note[0] ? note : "Can't reach the server");
                return;   /* not reached */
            }
            /* PENDING: reachable, just waiting for admin approval. Paint the
             * status only when we first enter the pending state, so the slow
             * panel isn't redrawn on every retry wake / USB dev-loop restart
             * while it stays pending. */
            if (rest_config_get_ui_state() != UI_PENDING) {
                if (note[0]) splash_show_message("Almost done", note);
                rest_config_set_ui_state(UI_PENDING);
            }
            sleep_forever_or_until_timer();
            return;
        }
        just_onboarded = true;   /* got a token this cycle */
        c = rest_config_get();
    }

    /* A manual button wake forces a repaint even if the frame is unchanged:
     * drop the cached ETag so rest_get_frame() omits If-None-Match and the
     * server returns 200 (full frame) instead of 304. In-memory only -- not
     * persisted, so the next timer wake resumes normal 304 dedup. */
    if (woke_by_button) rest_config_set_frame_etag("");

    /* 2. Frame metadata, with If-None-Match for the ETag/304 dedup. */
    rest_frame_out_t fo;
    rest_status_t fs = rest_get_frame(&fo, 10000);
    if (fs == REST_OK) {
        snprintf(new_etag, sizeof new_etag, "%s", fo.etag);
        /* The 200 body is the freshest button_wake_s (a 304/204 has no body:
         * the NVS-cached value from the last status/register stands). */
        if (fo.button_wake_s >= 0 &&
            fo.button_wake_s != rest_config_get()->button_wake_s) {
            rest_config_set_button_wake_s(fo.button_wake_s);
            cfg_dirty = true;
        }
        char fullurl[512];
        resolve_url(c->server_url, fo.url, fullurl, sizeof fullurl);
        fetched_image_t img;
        if (image_fetch(fullurl, &img) == ESP_OK) {
            if (image_decode_to_frame(&img, fullurl, &frame) != ESP_OK) frame = NULL;
            free(img.data);
        } else {
            ESP_LOGE(TAG, "frame fetch failed for %s", fullurl);
        }
    } else if (fs == REST_NOT_MODIFIED) {
        ESP_LOGI(TAG, "frame unchanged (304); skipping paint");
        skip_paint = true;
    } else if (fs == REST_NO_CONTENT) {
        ESP_LOGI(TAG, "no frame rendered yet (204); skipping paint");
        skip_paint = true;
    } else if (fs == REST_UNAUTH || fs == REST_FORBIDDEN) {
        ESP_LOGW(TAG, "frame auth failed (%d); wiping token to re-register", fs);
        rest_config_set_device_token("");
        cfg_dirty = true;
    } else {
        ESP_LOGW(TAG, "frame request failed (%d); keeping last image", fs);
    }

    /* 3. Status heartbeat (only while we still hold a token). next_poll_s from
     * the response drives this cycle's deep sleep. */
    if (rest_config_get()->device_token[0] != '\0') {
        char ip[16] = {0};
        wifi_manager_get_sta_ip(ip, sizeof ip);
        int32_t  interval = rest_config_get()->sleep_s;
        time_t   now      = time(NULL);
        uint32_t sleep_until = (now > EPOCH_REASONABLE_MIN &&
                                now < EPOCH_REASONABLE_MAX && interval > 0)
                                   ? (uint32_t)(now + interval) : 0;
        rest_status_out_t so;
        rest_status_t ss = rest_post_status(current_rssi(), ip, pw, ph,
                                            interval, sleep_until, FW_VERSION, &so, 8000);
        if (ss == REST_OK) {
            if (so.sleep_interval_s > 0 && so.sleep_interval_s != rest_config_get()->sleep_s) {
                rest_config_set_sleep_s(so.sleep_interval_s);
                cfg_dirty = true;
            }
            if (so.next_poll_s > 0) rest_config_set_sleep_s(so.next_poll_s);  /* drives this sleep */
            if (so.button_wake_s >= 0 &&
                so.button_wake_s != rest_config_get()->button_wake_s) {
                rest_config_set_button_wake_s(so.button_wake_s);
                cfg_dirty = true;
                ESP_LOGI(TAG, "button wake window config: %lds",
                         (long)rest_config_get()->button_wake_s);
            }
#if BOARD_HAS_TOUCH
            /* Touch config arrives in the same "config" object as sleep_interval_s.
             * -1 means the field was absent; keep the current value then. */
            if (so.touch_enabled >= 0 || so.touch_linger_s >= 0) {
                const rest_config_t *tc = rest_config_get();
                bool    en  = (so.touch_enabled  >= 0) ? (so.touch_enabled != 0) : tc->touch_enabled;
                int32_t lin = (so.touch_linger_s >= 0) ? so.touch_linger_s       : tc->touch_linger_s;
                if (en != tc->touch_enabled || lin != tc->touch_linger_s) {
                    rest_config_set_touch(en, lin);
                    cfg_dirty = true;
                    ESP_LOGI(TAG, "touch config: enabled=%d linger=%lds", en, (long)lin);
                }
            }
#endif
            /* Deck resync signal: decided here, executed at the tail of the
             * wake (after painting + reporting, radio still up). */
            if (so.deck_present) {
                snprintf(deck_srv_ver, sizeof deck_srv_ver, "%s", so.deck_version);
                deck_sync_needed = deck_sync_pending(true, deck_srv_ver);
            }
#if TESSERAE_OTA_CAPABILITY_ENABLED
            if (so.ota_present) {
                if (so.ota_reason == OTA_VERIFY_OK) {
                    char attempt[8];
                    snprintf(attempt, sizeof attempt, "%02x%02x%02x",
                             so.ota_manifest.sha256[0], so.ota_manifest.sha256[1],
                             so.ota_manifest.sha256[2]);
                    ota_report_set(OTA_REPORT_DOWNLOADING, "",
                                   so.ota_manifest.fw_version, attempt);
                    ota_install_result_t install =
                        ota_install_apply(&so.ota_manifest);
                    if (install == OTA_INSTALL_APPLIED) {
                        /* Persist BEFORE the reboot: the next boot resolves
                         * pending_confirm into confirmed or rolled_back. */
                        ota_report_set(OTA_REPORT_PENDING_CONFIRM, "",
                                       so.ota_manifest.fw_version, attempt);
                        if (cfg_dirty) rest_config_save();
                        free(frame);
                        frame = NULL;
                        ESP_LOGI(TAG, "rebooting into verified OTA image");
                        esp_restart();
                    }
                    ota_report_install_failure(&so.ota_manifest, install, attempt);
                    ESP_LOGW(TAG, "OTA install failed: %s",
                             ota_install_result_name(install));
                } else {
                    /* Verify-time refusals are `rejected`; the verifier's
                     * reason names are the contract vocabulary. The manifest
                     * is only populated for post-parse refusals (kind
                     * mismatch / already current). */
                    ota_report_set(OTA_REPORT_REJECTED,
                                   ota_verify_reason_name(so.ota_reason),
                                   so.ota_manifest.fw_version, NULL);
                    ESP_LOGW(TAG, "OTA descriptor rejected: %s",
                             ota_verify_reason_name(so.ota_reason));
                }
            }
#endif
        } else {
            ESP_LOGW(TAG, "status post failed (%d)", ss);
            if (ss == REST_UNAUTH || ss == REST_FORBIDDEN) {
                rest_config_set_device_token(""); cfg_dirty = true;
            }
        }
    }

#if BOARD_HAS_TOUCH
    /* WiFi is up and we hold a token: replay any strokes queued from earlier
     * wakes whose connect had failed (dispatched, not painted). */
    if (rest_config_get()->device_token[0] != '\0' && touch_queue_count() > 0)
        touch_queue_flush();
#endif

    /* 4. Radio down before the slow (~30 s) refresh -- the biggest battery
     * saving in the render path (~80 mA otherwise). A touch wake with a linger
     * window, or a button wake with a button_wake_s window (issue #123), keeps
     * WiFi up instead so repeated interactions stay responsive: re-fetching a
     * page needs the radio, and a few seconds of it is cheaper than a full
     * reconnect per press. Timer/scheduled wakes never linger. */
#if BOARD_HAS_TOUCH
    will_linger = woke_by_touch && rest_config_get()->touch_linger_s > 0;
#endif
#ifdef BOARD_HAS_BUTTONS
    will_linger = will_linger ||
                  (woke_by_button && rest_config_get()->button_wake_s > 0);
#endif
    /* A pending deck sync keeps the radio up through the paint so the sync
     * tail can run afterwards (contract: sync after painting + reporting). */
    if (!will_linger && !deck_sync_needed) wifi_sta_stop();

    if (frame != NULL) {
        ESP_LOGI(TAG, "painting downloaded frame (~30 s)...");
        ESP_ERROR_CHECK(epd_port_init());
        epd_init();
        epd_display(frame);
        epd_sleep();
        free(frame);
        if (new_etag[0]) { rest_config_set_frame_etag(new_etag); cfg_dirty = true; }
        rest_config_set_ui_state(UI_CONNECTED);   /* a real frame is up now */
        deck_network_painted();   /* SD-paint report no longer describes the display */
    } else if (just_onboarded) {
        /* Onboarding completed but the server has no frame ready yet -- confirm
         * the successful connect once, on the transition, so setup has clear
         * closure (the frame lands on a later wake) without redrawing each loop. */
        if (rest_config_get_ui_state() != UI_CONNECTED) {
            ESP_LOGI(TAG, "onboarded, no frame yet; painting connected splash");
            splash_show_message("Connected!", "Waiting for your first frame");
            rest_config_set_ui_state(UI_CONNECTED);
        }
    } else if (skip_paint) {
        /* 304/204: nothing changed, leave the current image */
    } else if (rest_config_get()->last_frame_etag[0] != '\0') {
        ESP_LOGI(TAG, "no frame this cycle; keeping last image");
    } else {
        ESP_LOGI(TAG, "no frame yet; leaving panel as-is");
    }

    /* Deck cache sync tail (contract: AFTER painting and reporting): the
     * radio was kept up for this; fetch the manifest, diff, fetch missing
     * frames, delete orphans. Then finish the deferred radio-down. */
    if (deck_sync_needed) {
        deck_sync_tail(true, deck_srv_ver);
        if (!will_linger) wifi_sta_stop();
    }

#if BOARD_HAS_TOUCH
    /* Touch linger: stay awake touch_linger_s after the interaction, polling the
     * GT911 INT and firing further touch GETs at full responsiveness (no deep
     * sleep + boot + reconnect between rapid touches). WiFi is still up here.
     * The window resets on each interaction; it ends when idle for the window. */
    if (will_linger) {
        int linger_s = rest_config_get()->touch_linger_s;
        ESP_LOGI(TAG, "touch linger: up to %d s awake for further touches", linger_s);
        int64_t deadline = esp_timer_get_time() + (int64_t)linger_s * 1000000;
        while (esp_timer_get_time() < deadline) {
            if (!touch_int_asserted()) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            touch_stroke_t st;
            touch_capture_stroke(&st, TOUCH_FIRST_POINT_MS, TOUCH_CAP_MS);
            if (!st.valid) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            uint32_t ev = ++s_button_event_seq;
            ESP_LOGI(TAG, "linger touch (%d,%d)->(%d,%d) %ums (event %u)",
                     st.x0, st.y0, st.x1, st.y1, (unsigned)st.ms, (unsigned)ev);
            rest_set_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                           rest_config_get()->last_frame_etag, ev);
            if (fetch_and_paint_current(rest_config_get()->server_url)) cfg_dirty = true;
            deadline = esp_timer_get_time() + (int64_t)linger_s * 1000000;   /* reset */
        }
        rest_set_touch(0, 0, 0, 0, 0, NULL, 0);   /* clear pending touch */
        wifi_sta_stop();
    }
#endif

#ifdef BOARD_HAS_BUTTONS
    /* Post-button stay-awake window (issue #123): after a button wake's paint,
     * stay awake with WiFi up for button_wake_s, polling for further presses.
     * Each press re-fetches ?button=...&button_event_id=<n> with a fresh event
     * id, paints,
     * and RESETS the countdown, so continuous scrolling keeps it awake. The
     * window elapsing resumes the normal sleep_interval_s cadence. A hard cap
     * bounds total awake time client-side (a faulty bouncing button otherwise
     * could re-trigger indefinitely; a merely held/stuck one fires only one
     * edge in buttons_poll_pressed()). */
    if (woke_by_button && rest_config_get()->button_wake_s > 0) {
        int32_t win_s = rest_config_get()->button_wake_s;
        ESP_LOGI(TAG, "button wake window: up to %ld s awake for further presses",
                 (long)win_s);
        buttons_poll_init();
        int64_t hard_cap = esp_timer_get_time() + BUTTON_WINDOW_CAP_S * 1000000LL;
        int64_t deadline = esp_timer_get_time() + (int64_t)win_s * 1000000;
        while (esp_timer_get_time() < deadline && esp_timer_get_time() < hard_cap) {
            button_id_t b = buttons_poll_pressed();
            if (b == BTN_NONE) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            uint32_t ev = ++s_button_event_seq;
            ESP_LOGI(TAG, "window press '%s' (event %u)", button_name(b), (unsigned)ev);
            rest_set_button(button_name(b), ev);
            /* Like the wake press: a manual press forces a repaint (200, not 304). */
            rest_config_set_frame_etag("");
            if (fetch_and_paint_current(rest_config_get()->server_url)) cfg_dirty = true;
            /* Adopt the freshest window value (the /frame 200 body may have just
             * changed it); 0 now means the admin turned the window off. */
            win_s = rest_config_get()->button_wake_s;
            if (win_s <= 0) break;
            deadline = esp_timer_get_time() + (int64_t)win_s * 1000000;
        }
        rest_set_button(NULL, 0);   /* clear so it doesn't leak into later requests */
        wifi_sta_stop();
    }
#endif

    /* 5. Persist any config changes, then sleep (interval from rest_config). */
    if (cfg_dirty) {
        ESP_LOGI(TAG, "config %s", rest_config_save() == ESP_OK ? "saved" : "SAVE FAILED");
    }
    sleep_forever_or_until_timer();
}
