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
#include "buttons.h"        /* front-button wake/report (header-only; no-op if none) */
#include "touch_gt911.h"    /* GT911 touch wake (guarded by BOARD_HAS_TOUCH) */
#include "battery.h"
#include "epd_driver.h"
#include "image_decoder.h"
#include "image_fetcher.h"
#include "net_rest.h"
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

#if BOARD_HAS_TOUCH
/* Fetch the current frame and paint it if the server returned a new one. Used
 * inside the touch linger loop, where WiFi is kept up so repeated touches don't
 * pay reconnect/boot latency. The touch params must already be set on the REST
 * client (rest_set_touch). Returns true if a new frame was painted. */
static bool touch_fetch_and_paint(const char *server_url)
{
    rest_frame_out_t fo;
    if (rest_get_frame(&fo, 8000) != REST_OK) return false;   /* 304/204/err: nothing new */
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

    ESP_ERROR_CHECK(wifi_manager_init());
    rest_config_load();

#if BOARD_HAS_TOUCH
    bool will_linger = false;
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
    if (woke_by_touch) {
        if (touch_init() == ESP_OK) {
            touch_stroke_t st;
            touch_capture_stroke(&st, TOUCH_FIRST_POINT_MS, TOUCH_CAP_MS);
            if (st.valid) {
                uint32_t ev = ++s_button_event_seq;   /* shares the wake-event counter */
                ESP_LOGI(TAG, "touch (%d,%d)->(%d,%d) %ums (event %u)",
                         st.x0, st.y0, st.x1, st.y1, (unsigned)st.ms, (unsigned)ev);
                rest_set_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                               rest_config_get()->last_frame_etag, ev);
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
        } else {
            ESP_LOGW(TAG, "status post failed (%d)", ss);
            if (ss == REST_UNAUTH || ss == REST_FORBIDDEN) {
                rest_config_set_device_token(""); cfg_dirty = true;
            }
        }
    }

    /* 4. Radio down before the slow (~30 s) refresh -- the biggest battery
     * saving in the render path (~80 mA otherwise). A touch wake with a linger
     * window keeps WiFi up instead, so repeated touches stay responsive. */
#if BOARD_HAS_TOUCH
    will_linger = woke_by_touch && rest_config_get()->touch_linger_s > 0;
    if (!will_linger) wifi_sta_stop();
#else
    wifi_sta_stop();
#endif

    if (frame != NULL) {
        ESP_LOGI(TAG, "painting downloaded frame (~30 s)...");
        ESP_ERROR_CHECK(epd_port_init());
        epd_init();
        epd_display(frame);
        epd_sleep();
        free(frame);
        if (new_etag[0]) { rest_config_set_frame_etag(new_etag); cfg_dirty = true; }
        rest_config_set_ui_state(UI_CONNECTED);   /* a real frame is up now */
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
            if (touch_fetch_and_paint(rest_config_get()->server_url)) cfg_dirty = true;
            deadline = esp_timer_get_time() + (int64_t)linger_s * 1000000;   /* reset */
        }
        rest_set_touch(0, 0, 0, 0, 0, NULL, 0);   /* clear pending touch */
        wifi_sta_stop();
    }
#endif

    /* 5. Persist any config changes, then sleep (interval from rest_config). */
    if (cfg_dirty) {
        ESP_LOGI(TAG, "config %s", rest_config_save() == ESP_OK ? "saved" : "SAVE FAILED");
    }
    sleep_forever_or_until_timer();
}
