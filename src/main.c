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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
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

/* Increment on each manual reset; two within one wake window => settings mode.
 * The window is closed by zeroing the counter when we commit to deep sleep
 * (see sleep_forever_or_until_timer), so single taps minutes apart don't add
 * up to a false double-tap. */
static bool detect_settings_mode(esp_reset_reason_t reason)
{
    if (s_rtc_magic != RTC_TAP_MAGIC) {   /* power-on / garbage: seed it */
        s_rtc_magic = RTC_TAP_MAGIC;
        s_reset_taps = 0;
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

    int interval = effective_sleep_s();
    ESP_LOGI(TAG, "on battery; deep sleep for %d s%s",
             interval,
             (interval == SLEEP_INTERVAL_S) ? " (default)" : " (server-driven)");
    /* epd_sleep() already dropped the panel power rail; no extra cleanup
     * needed before going down. */
    esp_sleep_enable_timer_wakeup((uint64_t)interval * 1000000ULL);
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- app ---------- */

static void run_provisioning_then_reboot(void)
{
    ESP_LOGW(TAG, "no usable wifi creds; painting portal splash + captive portal");
    /* Paint logo + WPA QR before bringing up the AP so the user can scan
     * to join Tesserae-Setup instead of typing the SSID. ~30 s panel refresh
     * runs concurrently with the AP/HTTPD/DNS bringup that's about to happen. */
    splash_show_portal();
    esp_err_t err = provisioning_run_blocking();
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

/* First-boot onboarding: obtain a device token. Returns true once we hold one
 * (continue the cycle); false means "no token yet", with a backoff written to
 * rest_config's sleep_s for the caller to sleep on. Zero-touch discover/claim
 * by default (admin clicks Register in Tesserae, no typing on the device);
 * a stored pairing code opts into strict admin-gated register. Ported from
 * tesserae-device-pico-bin main.c rest_bootstrap(). */
static bool rest_bootstrap(uint16_t pw, uint16_t ph, const char *mac, bool *dirty)
{
    const rest_config_t *c = rest_config_get();
    if (c->device_token[0] != '\0') return true;   /* already bootstrapped */

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
            return true;
        }
        int32_t backoff = (c->sleep_s > 0) ? c->sleep_s : 900;
        if (rs == REST_UNAUTH || rs == REST_FORBIDDEN) {
            backoff = 3600;
            ESP_LOGW(TAG, "register rejected (%d); sleeping 1h to re-pair", rs);
        } else if (rs == REST_RATELIMIT) {
            backoff = (ro.retry_after_s > 0) ? ro.retry_after_s : 3600;
            ESP_LOGW(TAG, "register rate-limited; backoff %lds", (long)backoff);
        } else {
            ESP_LOGW(TAG, "register failed (%d); retry next cycle", rs);
        }
        rest_config_set_sleep_s(backoff);
        return false;
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
        return true;
    }
    int32_t backoff;
    if (ds == REST_OK) {            /* registered:false, awaiting admin Register */
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 30;
        ESP_LOGI(TAG, "discovered, waiting for admin to Register; retry in %lds",
                 (long)backoff);
    } else if (ds == REST_RATELIMIT) {
        backoff = (dd.retry_after_s > 0) ? dd.retry_after_s : 60;
        ESP_LOGW(TAG, "discover rate-limited; backoff %lds", (long)backoff);
    } else {
        backoff = 30;   /* unreachable (e.g. wrong server URL): retry soon */
        ESP_LOGW(TAG, "discover failed (%d); retry in %lds", ds, (long)backoff);
    }
    rest_config_set_sleep_s(backoff);
    return false;
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool settings_mode = detect_settings_mode(reset_reason);
    ESP_LOGI(TAG, "boot; reset_reason=%d wakeup_cause=%d settings_mode=%d",
             reset_reason, esp_sleep_get_wakeup_cause(), settings_mode);

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
    ESP_LOGW(TAG, "EPD_SELFTEST: done; halting. Press RESET to repeat.");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

    ESP_ERROR_CHECK(wifi_manager_init());
    rest_config_load();

    /* Skip the 30 s splash when entering settings mode -- the user is waiting
     * on the editor, not a panel sanity check. */
    bool have_creds = wifi_creds_present();
    if (!settings_mode) {
        maybe_show_splash(reset_reason, have_creds);
    }

    /* The captive portal collects both WiFi and the Tesserae server URL, so a
     * device missing either is not yet usable -- send it to provisioning. */
    if (!have_creds || !rest_config_has_server()) {
        if (have_creds)
            ESP_LOGW(TAG, "wifi set but no Tesserae server URL; opening portal");
        run_provisioning_then_reboot();
        return;
    }

    esp_err_t err = wifi_sta_connect_stored();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA connect failed (%s); falling back to portal",
                 esp_err_to_name(err));
        run_provisioning_then_reboot();
        return;
    }

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

    bool cfg_dirty  = false;
    bool skip_paint = false;
    uint8_t *frame  = NULL;
    char new_etag[80] = {0};

    /* 1. Bootstrap a device token (discover/claim, or register with a code). */
    if (c->device_token[0] == '\0') {
        if (!rest_bootstrap(pw, ph, mac, &cfg_dirty)) {
            /* No token yet; rest_config sleep_s holds the backoff. The common
             * "waiting for admin" retry is not persisted (avoid flash wear);
             * only persist if a code register/adopt changed something. */
            if (cfg_dirty) rest_config_save();
            wifi_sta_stop();
            sleep_forever_or_until_timer();
            return;
        }
        c = rest_config_get();
    }

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
        } else {
            ESP_LOGW(TAG, "status post failed (%d)", ss);
            if (ss == REST_UNAUTH || ss == REST_FORBIDDEN) {
                rest_config_set_device_token(""); cfg_dirty = true;
            }
        }
    }

    /* 4. Radio down before the slow (~30 s) refresh -- the biggest battery
     * saving in the render path (~80 mA otherwise). */
    wifi_sta_stop();

    if (skip_paint) {
        /* nothing to paint */
    } else if (frame != NULL) {
        ESP_LOGI(TAG, "painting downloaded frame (~30 s)...");
        ESP_ERROR_CHECK(epd_port_init());
        epd_init();
        epd_display(frame);
        epd_sleep();
        free(frame);
        if (new_etag[0]) { rest_config_set_frame_etag(new_etag); cfg_dirty = true; }
    } else if (rest_config_get()->last_frame_etag[0] != '\0') {
        ESP_LOGI(TAG, "no frame this cycle; keeping last image");
    } else {
        ESP_LOGI(TAG, "no frame yet; leaving panel as-is");
    }

    /* 5. Persist any config changes, then sleep (interval from rest_config). */
    if (cfg_dirty) {
        ESP_LOGI(TAG, "config %s", rest_config_save() == ESP_OK ? "saved" : "SAVE FAILED");
    }
    sleep_forever_or_until_timer();
}
