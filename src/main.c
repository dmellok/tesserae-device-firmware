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
#include "esp_netif_sntp.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "app_config.h"
#include "epd_driver.h"
#include "heartbeat.h"
#include "image_decoder.h"
#include "image_fetcher.h"
#include "mqtt_handler.h"
#include "provisioning.h"
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

/* ---------- "should I bother re-rendering?" ---------- */

static void sha256_hex(const char *in, char out_hex[65])
{
    uint8_t digest[32];
    mbedtls_sha256((const unsigned char *)in, strlen(in), digest, 0);
    for (int i = 0; i < 32; i++) {
        static const char H[] = "0123456789abcdef";
        out_hex[i*2]   = H[(digest[i] >> 4) & 0xF];
        out_hex[i*2+1] = H[digest[i] & 0xF];
    }
    out_hex[64] = '\0';
}

static bool hash_matches_stored(const char *hash)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) != ESP_OK) return false;
    char prev[65] = {0};
    size_t len = sizeof(prev);
    esp_err_t err = nvs_get_str(h, NVS_KEY_LAST_HASH, prev, &len);
    nvs_close(h);
    return err == ESP_OK && strcmp(prev, hash) == 0;
}

static void store_hash(const char *hash)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_LAST_HASH, hash);
    nvs_commit(h);
    nvs_close(h);
}

/* Read the MQTT-configured sleep interval from NVS, falling back to the
 * compile-time SLEEP_INTERVAL_S default. Clamped defensively in case
 * something wrote a bad value before the bounds check existed. */
static int load_sleep_interval_s(void)
{
    int32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_i32(h, NVS_KEY_SLEEP_S, &v);
        nvs_close(h);
        if (err == ESP_OK &&
            v >= SLEEP_INTERVAL_MIN_S &&
            v <= SLEEP_INTERVAL_MAX_S) {
            return v;
        }
    }
    return SLEEP_INTERVAL_S;
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

    int interval = load_sleep_interval_s();
    ESP_LOGI(TAG, "on battery; deep sleep for %d s%s",
             interval,
             (interval == SLEEP_INTERVAL_S) ? " (default)" : " (set via mqtt)");
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

/* ---------- wall-clock time + heartbeat ---------- */

/* Lazy NTP sync. The RTC slow clock keeps running through deep sleep, so the
 * system time set by SNTP normally persists across wakes -- timer wakes can
 * skip the round-trip. But if a previous sync ever landed badly (router
 * intercepting NTP, transient pool.ntp.org weirdness), that error would
 * propagate forever, so cold boots (POWERON / EXT reset) always force a
 * fresh sync regardless of what the RTC currently thinks. That gives the
 * user a recovery path: power-cycle or RESET the device. */
static void ensure_time_synced(int wait_ms, bool force_resync)
{
    if (!force_resync && time(NULL) > 1700000000LL) return;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp init failed: %s; sleep_until will be omitted",
                 esp_err_to_name(err));
        return;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(wait_ms));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ntp synced; epoch=%lld", (long long)time(NULL));
    } else {
        ESP_LOGW(TAG, "ntp sync timeout (%dms); sleep_until will be omitted",
                 wait_ms);
    }
    esp_netif_sntp_deinit();
}

/* Format + publish the heartbeat right before sleep so sleep_until is
 * wall-clock-accurate (Tesserae's smart-sync wire contract). Caller must
 * have WiFi up; this function brings MQTT up one-shot, publishes retained
 * QoS 1, waits for the PUBACK, tears MQTT down. Failures are logged and
 * swallowed -- a missed heartbeat just means the server's next prediction
 * falls back to its tolerance-window math for one cycle.
 *
 * Defensive checks before shipping sleep_until:
 *   - sane-epoch window: now must look like a real wall-clock time, between
 *     2023-11 and 2039-09. Rejects the ESP-IDF default 2016 epoch when NTP
 *     has never synced, and rejects a misconfigured SNTP server returning a
 *     wild future date.
 *   - cross-check: (sleep_until - now) must equal sleep_s within +-5 s.
 *     Tautological with the current `now + sleep_s` math but catches future
 *     refactors that compute sleep_until any other way. Server-side
 *     Tesserae v0.43.1+ has its own fallback if these disagree by >30 s,
 *     but we'd rather not ship the wrong value in the first place. */
#define EPOCH_REASONABLE_MIN 1700000000LL   /* 2023-11-14 */
#define EPOCH_REASONABLE_MAX 2200000000LL   /* 2039-09-13 */

static void publish_heartbeat(int sleep_s, esp_reset_reason_t reset_reason)
{
    char hb[320];
    time_t now = time(NULL);
    time_t sleep_until = 0;

    if (now > EPOCH_REASONABLE_MIN && now < EPOCH_REASONABLE_MAX) {
        sleep_until = now + sleep_s;

        long delta = (long)(sleep_until - now);
        if (delta < (long)sleep_s - 5 || delta > (long)sleep_s + 5) {
            ESP_LOGW(TAG,
                "sleep_until cross-check failed: delta=%ld, sleep_s=%d; omitting",
                delta, sleep_s);
            sleep_until = 0;
        }
    } else if (now != 0) {
        ESP_LOGW(TAG,
            "wall-clock looks bogus (epoch=%lld); omitting sleep_until",
            (long long)now);
    }

    heartbeat_format_json(hb, sizeof hb, sleep_s, reset_reason, sleep_until);

    esp_err_t err = mqtt_publish_status(hb);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "heartbeat publish failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool settings_mode = detect_settings_mode(reset_reason);
    ESP_LOGI(TAG, "boot; reset_reason=%d wakeup_cause=%d settings_mode=%d",
             reset_reason, esp_sleep_get_wakeup_cause(), settings_mode);

    ESP_ERROR_CHECK(wifi_manager_init());

    /* Skip the 30 s splash when entering settings mode -- the user is waiting
     * on the editor, not a panel sanity check. */
    bool have_creds = wifi_creds_present();
    if (!settings_mode) {
        maybe_show_splash(reset_reason, have_creds);
    }

    if (!have_creds) {
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

    int sleep_s = load_sleep_interval_s();

    /* Force a fresh NTP sync on any cold boot so a previously-bad sync
     * (network-side issue, transient pool weirdness) is recoverable by
     * power-cycling or hitting RESET. Timer wakes from deep sleep reuse
     * the RTC's preserved time and skip the round-trip. If sync fails,
     * sleep_until is omitted from the heartbeat and the server's
     * smart-sync scheduler falls back to its tolerance-window math. */
    bool cold_boot = (reset_reason == ESP_RST_POWERON ||
                      reset_reason == ESP_RST_EXT);
    ensure_time_synced(5000, cold_boot);

    /* Fetch the retained URL only -- the heartbeat publishes at the END
     * (after paint, if any) so its sleep_until reflects the actual sleep
     * about to start instead of leading it by ~30 s of paint time. */
    mqtt_job_t job;
    err = mqtt_fetch_retained(&job);
    bool have_url = (err == ESP_OK && job.url[0]);
    if (!have_url) {
        ESP_LOGI(TAG, "no retained job (%s)", esp_err_to_name(err));
    }

    bool need_paint = false;
    char hash[65];
    if (have_url) {
        sha256_hex(job.url, hash);
        if (hash_matches_stored(hash)) {
            ESP_LOGI(TAG, "url unchanged since last render; skipping refresh");
        } else {
            need_paint = true;
        }
    }

    if (need_paint) {
        fetched_image_t img;
        err = image_fetch(job.url, &img);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fetch failed: %s", esp_err_to_name(err));
            publish_heartbeat(sleep_s, reset_reason);   /* wifi still up */
            wifi_sta_stop();
            sleep_forever_or_until_timer();
            return;
        }

        uint8_t *frame = NULL;
        err = image_decode_to_frame(&img, job.url, &frame);
        free(img.data);
        if (err != ESP_OK || !frame) {
            ESP_LOGE(TAG, "decode failed: %s", esp_err_to_name(err));
            publish_heartbeat(sleep_s, reset_reason);   /* wifi still up */
            wifi_sta_stop();
            sleep_forever_or_until_timer();
            return;
        }

        /* Free WiFi for the ~30 s panel refresh -- the single biggest
         * battery saving in the render path (~80 mA otherwise). */
        wifi_sta_stop();

        ESP_ERROR_CHECK(epd_port_init());
        epd_init();
        epd_display(frame);
        epd_sleep();
        free(frame);

        store_hash(hash);

        /* Bring WiFi back up just long enough to publish the final
         * heartbeat. ~3-5 s × ~80 mA = ~0.07-0.11 mAh extra per render
         * wake -- the cost of wall-clock-accurate sleep_until for
         * Tesserae's smart-sync scheduler. */
        ESP_LOGI(TAG, "render OK; reconnecting wifi for post-paint heartbeat");
        if (wifi_sta_connect_stored() == ESP_OK) {
            publish_heartbeat(sleep_s, reset_reason);
        } else {
            ESP_LOGW(TAG, "post-paint wifi reconnect failed; heartbeat skipped");
        }
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;
    }

    /* Hash-skip or no-URL path: WiFi+MQTT just came up for the fetch and
     * the radio is still on; publish heartbeat without a reconnect cycle. */
    publish_heartbeat(sleep_s, reset_reason);
    wifi_sta_stop();
    sleep_forever_or_until_timer();
}
