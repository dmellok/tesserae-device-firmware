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
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "nvs_flash.h"      /* factory-reset erase (20 s refresh-button hold) */
#include "buttons.h"          /* front-button wake/report (header-only; no-op if none) */
#include "power_latch.h"      /* battery self-latch (header-only; no-op if none) */
#include "buzzer.h"        /* piezo feedback on an input (no-op without a buzzer) */
#include "deck_run.h"       /* SD deck cache: local nav + sync (no-op w/o card) */
#include "collection_run.h" /* offline Album: SD playback + collection sync */
#include "overlay_run.h"    /* local overlay render mode (no-op w/o partial panel) */
#include "wake_align.h"     /* synchronized wake: absolute targets + drift trim */
#include "proto2_run.h"     /* protocol v2: device-owned touch (no-op w/o touch) */
#include "touch3_run.h"     /* touch v3: device-drawn primitives (no-op w/o touch) */
#include "sse_client.h"     /* proto2 push transport (kiosk mode only) */
#include "overlay.h"        /* pure overlay engine (used by OVERLAY_SELFTEST) */
#include "touch3.h"         /* pure touch-v3 engine (used by TOUCH3_SELFTEST) */
#include "deck_cache.h"     /* + sdcard.h/deck.h: DECK_SD_SELFTEST round trip */
#include "sdcard.h"
#include "sdmmc_cmd.h"      /* raw-sector benchmark in DECK_SD_SELFTEST */
#include "esp_heap_caps.h"
#include "touch_gt911.h"    /* GT911 touch wake (guarded by BOARD_HAS_TOUCH) */
#include "panel/epd_panel.h" /* epd_active_driver()->info.bpp (selftests) */
#include "touch_queue.h"    /* RTC replay queue for unsent touches (guarded) */
#include "touch_wakestub.h" /* RTC wake-stub early touch capture (guarded) */
#include "battery.h"
#include "ble_setup.h"
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
#include "relay.h"        /* cloud relay: remote panels */
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
RTC_NOINIT_ATTR static uint64_t s_button_event_seq;

/* One-shot deep-sleep interval override (seconds); 0 = use the server interval.
 * Set before sleep to schedule a shorter WiFi retry backoff. */
static int32_t s_sleep_override_s = 0;

/* proto2_run mints its /tap event ids off the same counter every other
 * input event uses (contract: one uint64 stream per boot). */
uint64_t app_next_event_id(void)
{
    return ++s_button_event_seq;
}

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
        /* Random seed, NOT zero: the server dedups on event_id ACROSS
         * reboots (device_facts persistence), so restarting the sequence at
         * 1 after a power-on/reflash replays already-consumed ids and every
         * button/touch action is silently dropped as a duplicate (bench
         * 2026-07-25: taps 304-acked but HA never fired). Random restart
         * points keep ids unique for any realistic history; within a boot
         * the sequence stays monotonic for retry dedup. */
        /* 52-bit random start point: ids travel as JSON numbers, so they
         * must stay <= 2^53 for the cJSON double round-trip to be exact
         * (proto2 /tap event_id). Monotonic within a boot as before. */
        s_button_event_seq = ((uint64_t)(esp_random() & 0xFFFFF) << 32)
                             | esp_random();
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

/* A downloaded, decoded frame that has not been put on the glass yet.
 *
 * Fetching and painting used to be one indivisible step, which was fine while
 * the only caller painted immediately. Always-on mode needs to hold a frame
 * when the panel's repaint floor has not elapsed, so the two halves are
 * separable -- see frame_fetch() / frame_paint() below, which are still
 * composed into fetch_and_paint_current() with nothing in between so that every
 * other caller behaves exactly as before. Defined here rather than beside them
 * because the always-on loop holds one by value across iterations. */
typedef struct {
    uint8_t          *frame;   /* malloc'd; owned by whoever holds this */
    rest_frame_out_t  fo;
} pending_frame_t;

/* ---------- deep sleep ---------- */

/* Always-on mode (server config always_on): never deep-sleep, hold the Wi-Fi
 * association, and poll on a short cadence.
 *
 * The point is reachability. Asleep, a device is unreachable, so a manual Send,
 * a schedule firing or a touch lands whenever the panel next happens to wake --
 * minutes, on the default cadence. Awake, it lands within awake_poll_s.
 *
 * Two cadences, deliberately independent:
 *   - FRAME POLLS run on next_poll_s (falling back to awake_poll_s). Cheap: one
 *     conditional GET that almost always 304s and touches nothing. The server
 *     pulls next_poll_s forward when it knows content is about to change, which
 *     is why we honour it rather than hardcoding awake_poll_s.
 *   - HEARTBEATS run on AWAKE_HEARTBEAT_S. They carry battery / RSSI / IP and
 *     drive the server's device card; at a 5 s poll cadence, sending one per
 *     poll would be pure noise. They also re-deliver config and server_time, so
 *     the clock re-syncs without an NTP client.
 *
 * Escape hatches, checked every iteration: config turns it off (adopted on the
 * next heartbeat, no reboot), or a visible cell runs down (see
 * power_battery_critical) -- then fall back to normal sleep cycles whatever the
 * server thinks, because protecting the pack outranks honouring the setting.
 *
 * Not board-specific: compiled for every board, offered to every board (see
 * power_can_stay_awake in battery.h). Touch, SSE and the overlay engine are
 * conditional inside. A queued OTA is still deliberately not applied mid-run;
 * it lands on the next reboot. */
static bool fetch_and_paint_current(const char *server_url);
static bool frame_fetch(const char *server_url, const char *held_etag,
                        pending_frame_t *out);
static void frame_paint(pending_frame_t *p);
static void frame_discard(pending_frame_t *p);

/* Poll cadence in seconds: the server's next_poll_s when it gave one, else
 * the configured awake cadence. Re-clamped here as well as in rest_config
 * because next_poll_s arrives on a different path and never passed through the
 * setter. */
static int32_t awake_poll_s(int32_t server_next_poll_s)
{
    int32_t v = server_next_poll_s > 0 ? server_next_poll_s
                                       : rest_config_get()->awake_poll_s;
    if (v < AWAKE_POLL_MIN_S) v = AWAKE_POLL_MIN_S;
    if (v > AWAKE_POLL_MAX_S) v = AWAKE_POLL_MAX_S;
    return v;
}

static void enter_ble_maintenance_from_awake_window(void);   /* defined below */

static void always_on_loop(void)
{
    ESP_LOGI(TAG, "always-on: staying awake (config always_on, poll %d s)",
             (int)rest_config_get()->awake_poll_s);
    if (wifi_sta_connect_stored() != ESP_OK)
        return;   /* no network: sleep normally, retry next wake */
#if BOARD_HAS_TOUCH
    /* Every touch board, not only the overlay ones: a panel that never sleeps
     * has no ext1 wake to catch a tap, so the loop below must poll the
     * digitiser itself or touch simply does nothing while always-on. */
    if (touch_init() != ESP_OK) ESP_LOGW(TAG, "always-on: GT911 init failed");
#endif
#ifdef BOARD_HAS_BUTTONS
    /* Likewise the front buttons: asleep they are ext1 wake sources, awake
     * they need polling, and without it a press (and its beep) goes nowhere. */
    buttons_poll_init();
#endif

    const int64_t US = 1000000;
    int64_t next_beat_us  = 0;      /* 0 = due now: beat once on entry */
    int64_t next_poll_us  = 0;
    /* Seeded with "now", not zero. The boot path fetches and paints the current
     * frame immediately before deciding to stay awake, so the glass has just
     * been driven when we get here. Starting at zero makes the loop believe it
     * has never painted, and its first repaint then follows the boot paint
     * instantly: observed on hardware as two full refreshes 10 s apart under a
     * 30 s floor. The panel does not care which code path drove it.
     *
     * When the boot path did not paint, the cost is that the first change waits
     * out one floor, which is the guarantee being made anyway. */
    int64_t last_paint_us = esp_timer_get_time();
    int64_t last_ok_us    = esp_timer_get_time();   /* stall guard */
    int64_t next_heap_us  = esp_timer_get_time() + (int64_t)AWAKE_HEAP_LOG_S * US;
    int64_t next_defer_log_us = 0;
    int32_t server_next_poll_s = -1;
    int      reconnect_backoff_s = 1;
    /* A fetched frame waiting for the panel's refresh floor to elapse. At most
     * one: a newer fetch replaces it rather than queueing behind it. */
    pending_frame_t pending = {0};

    for (;;) {
        const rest_config_t *c = rest_config_get();
        /* Server config, re-read every loop: the heartbeat below adopts the
         * latest config.always_on, so turning it off takes effect on the next
         * poll with no reboot. */
        if (!c->always_on) { ESP_LOGI(TAG, "always-on off (config)"); break; }
        if (power_battery_critical()) {
            ESP_LOGW(TAG, "always-on: battery %d%%; resuming sleep cycles",
                     battery_read_pct());
            break;
        }

        int64_t now = esp_timer_get_time();

        /* Association health. An AP reboot, a DHCP lease change or a roam must
         * not need a device reset, and must not hot-loop either: back off to a
         * cap and keep trying. Everything below needs the link, so restart the
         * iteration rather than issuing requests that are certain to fail. */
        char ip[16] = {0};
        if (!wifi_manager_get_sta_ip(ip, sizeof ip) || ip[0] == '\0') {
            ESP_LOGW(TAG, "always-on: link down; reconnecting in %d s",
                     reconnect_backoff_s);
            vTaskDelay(pdMS_TO_TICKS(reconnect_backoff_s * 1000));
            if (wifi_sta_connect_stored() == ESP_OK) {
                ESP_LOGI(TAG, "always-on: link back");
                reconnect_backoff_s = 1;
            } else if (reconnect_backoff_s < 60) {
                reconnect_backoff_s *= 2;
                if (reconnect_backoff_s > 60) reconnect_backoff_s = 60;
            }
            /* A long outage must still reboot rather than sit here for ever. */
            if (now - last_ok_us > (int64_t)AWAKE_STALL_REBOOT_S * US) {
                ESP_LOGE(TAG, "always-on: no successful exchange in %d s; "
                              "restarting", AWAKE_STALL_REBOOT_S);
                esp_restart();
            }
            continue;
        }

#ifdef BOARD_HAS_BUTTONS
        {
            button_id_t b = buttons_poll_pressed();
            if (b != BTN_NONE) {
                if (buttons_is_maintenance_button(b) &&
                    buttons_maintenance_held_for_activation()) {
                    enter_ble_maintenance_from_awake_window();
                    return;   /* not reached */
                }
                uint64_t ev = ++s_button_event_seq;
                ESP_LOGI(TAG, "always-on press '%s' (event %llu)", button_name(b),
                         (unsigned long long)ev);
                rest_set_button(button_name(b), ev);
                buzzer_feedback();
                /* A manual press forces a repaint (200, not 304), as on a wake. */
                rest_config_set_frame_etag("");
                if (fetch_and_paint_current(c->server_url)) {
                    rest_config_save();
                    last_paint_us = esp_timer_get_time();
                    if (pending.frame) frame_discard(&pending);
                }
                rest_set_button(NULL, 0);
                continue;
            }
        }
#endif

#if BOARD_HAS_TOUCH
        if (touch_int_asserted()) {
            touch_stroke_t st;
            /* The sample callback tracks a v3 slider live while the finger is
             * down; it is a no-op for every other primitive and when no v3
             * spec is held. */
            touch_capture_stroke_cb(&st, TOUCH_FIRST_POINT_MS, TOUCH_CAP_MS,
                                    touch3_stroke_sample, NULL);
            /* Touch-driven repaints COUNT toward the floor but are never
             * deferred by it. Deferring the response to a finger would be a
             * worse bug than driving the glass slightly hard: the whole reason
             * this device stays awake is that a tap lands in about a second.
             * They still stamp last_paint_us, so a poll-driven repaint cannot
             * follow one immediately and double up on the panel. */
            if (st.valid) {
                buzzer_feedback();   /* same reason as the wake path (#258) */
                bool p2_poll = false;
                bool painted = false;
                if (touch3_try_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                                     &p2_poll)) {
                    if (p2_poll) painted = fetch_and_paint_current(c->server_url);
                } else if (proto2_try_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                                            &p2_poll)) {
                    if (p2_poll) painted = fetch_and_paint_current(c->server_url);
                } else {
                    overlay_try_echo(st.x1, st.y1);
                    uint64_t ev = ++s_button_event_seq;
                    ESP_LOGI(TAG, "always-on touch (%d,%d)->(%d,%d) %ums (event %llu)",
                             st.x0, st.y0, st.x1, st.y1, (unsigned)st.ms,
                             (unsigned long long)ev);
                    rest_set_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                                   c->last_frame_etag, ev);
                    painted = fetch_and_paint_current(c->server_url);
                    rest_set_touch(0, 0, 0, 0, 0, NULL, 0);
                }
                if (painted) {
                    last_paint_us = esp_timer_get_time();
                    /* Whatever was queued is now stale: the tap fetched and
                     * painted something newer. */
                    if (pending.frame) frame_discard(&pending);
                }
            }
            continue;
        }
#endif /* BOARD_HAS_TOUCH */

#if defined(BOARD_HAS_TOUCH) && defined(BOARD_OVERLAY_PARTIAL)
        sse_pump(150);
        if (!sse_connected()) overlay_linger_poll();   /* polling fallback */
        proto2_flush_reports();
        touch3_flush_reports();
        proto2_linger_tick();
        if (overlay_take_refetch()) {
            rest_config_set_frame_etag("");
            rest_config_save();
            if (fetch_and_paint_current(c->server_url))
                last_paint_us = esp_timer_get_time();
        }
        /* A superseded frame is ordinary new content, so it goes through the
         * normal poll rather than painting here: the poll is conditional and
         * the digest really did change, so it returns 200 without dropping
         * the ETag, and the queued frame still waits on the refresh floor. */
        if (overlay_take_stale()) next_poll_us = 0;
        if (proto2_sync_pending()) proto2_sync_tail();
#endif /* touch + overlay */

        /* ---- frame poll ----
         * Polling and repainting are limited by different things and must not
         * be conflated. A poll is a conditional GET that answers 304 while the
         * frame is unchanged and never touches the glass, so it runs on
         * schedule, always. The panel's refresh floor bounds the PAINT only,
         * below.
         *
         * Deliberately NOT a stall-guard input: frame_fetch() returns false for
         * a 304 and for a hard network error alike, so treating it as liveness
         * would make a total outage look healthy. The heartbeat reports its
         * status unambiguously and is what feeds last_ok_us. */
        now = esp_timer_get_time();
        if (now >= next_poll_us) {
            pending_frame_t fresh;
            if (frame_fetch(c->server_url,
                            pending.frame ? pending.fo.etag : NULL, &fresh)) {
                if (pending.frame) {
                    /* Superseded before it ever reached the glass. Replace
                     * rather than keep the older one: the floor is about how
                     * fast the panel may be driven, not about painting stale
                     * content, so the panel must always land on the newest
                     * frame. */
                    ESP_LOGI(TAG, "always-on: held frame superseded (%s -> %s)",
                             pending.fo.etag, fresh.fo.etag);
                    frame_discard(&pending);
                }
                pending = fresh;
            }
            next_poll_us = esp_timer_get_time() +
                           (int64_t)awake_poll_s(server_next_poll_s) * US;
        }

        /* ---- repaint, bounded by the panel's own floor ----
         * The server no longer bounds repaint rate (it used to, by accident,
         * via a slow poll cadence), and it never knew this panel's limit
         * anyway. The device does, so the guarantee lives here.
         *
         * A deferred paint and a missed Send look identical from the operator's
         * side, so a held frame is always logged with the remaining wait. It is
         * never dropped. */
        now = esp_timer_get_time();
        if (pending.frame) {
            int64_t since = now - last_paint_us;
            if (since >= (int64_t)AWAKE_MIN_REPAINT_S * US) {
                frame_paint(&pending);
                last_paint_us = esp_timer_get_time();
            } else if (now >= next_defer_log_us) {
                int wait_s = (int)(((int64_t)AWAKE_MIN_REPAINT_S * US - since)
                                   / US) + 1;
                ESP_LOGI(TAG, "always-on: holding frame %s for %d s "
                              "(panel refresh floor %d s)",
                         pending.fo.etag, wait_s, AWAKE_MIN_REPAINT_S);
                next_defer_log_us = now + US;   /* at most once a second */
            }
        }

        /* ---- heartbeat ----
         * Its own, much slower clock. Carries telemetry, and brings back config
         * (always_on, awake_poll_s, sleep_interval_s), next_poll_s and
         * server_time -- which is how the clock re-syncs, there being no NTP
         * client in this firmware. */
        now = esp_timer_get_time();
        if (now >= next_beat_us) {
            rest_status_out_t so;
            /* next_sleep_s / sleep_until are suppressed inside rest_post_status
             * while always_on; the 0 here is just the unused argument. */
            if (rest_post_status(current_rssi(), ip, EPD_WIDTH, EPD_HEIGHT,
                                 0, 0, FW_VERSION, &so, 8000) == REST_OK) {
#if defined(BOARD_HAS_TOUCH) && defined(BOARD_OVERLAY_PARTIAL)
                if (so.overlay_values[0]) {
                    overlay_ingest_values(so.overlay_values,
                                          strlen(so.overlay_values));
                    proto2_ingest_values(so.overlay_values,
                                         strlen(so.overlay_values));
                    touch3_ingest_values(so.overlay_values,
                                         strlen(so.overlay_values));
                }
                if (so.overlay_patches[0])
                    overlay_ingest_patches(so.overlay_patches,
                                           strlen(so.overlay_patches));
                proto2_note_clock(so.server_time, so.local_hh, so.local_mm);
                if (so.sync_obj[0])
                    proto2_note_sync(so.sync_obj, strlen(so.sync_obj));
#endif
                /* Keep the deep-sleep cadence current even though we are not
                 * using it: it is what we fall back to the instant always_on
                 * goes false, and it must not be stale when that happens. */
                if (so.sleep_interval_s > 0)
                    rest_config_set_sleep_s(so.sleep_interval_s);
                server_next_poll_s = so.next_poll_s;
                last_ok_us = esp_timer_get_time();
                /* The cadence actually in force, and where it came from. Worth
                 * a line per minute: next_poll_s overrides the configured
                 * awake cadence, so when a panel polls slower than Settings
                 * says, this is the difference between a firmware bug and the
                 * server still deriving the value from the sleep interval. */
                ESP_LOGI(TAG, "always-on: next_poll_s=%d, awake_poll_s=%d "
                              "-> polling every %d s",
                         (int)so.next_poll_s,
                         (int)rest_config_get()->awake_poll_s,
                         (int)awake_poll_s(server_next_poll_s));
            }
            next_beat_us = esp_timer_get_time() + (int64_t)AWAKE_HEARTBEAT_S * US;
        }

        /* ---- long-uptime guards ----
         * Deep sleep used to reset this device every cycle and hide leaks; it
         * may now run for months. */
        now = esp_timer_get_time();
        if (now >= next_heap_us) {
            ESP_LOGI(TAG, "always-on: uptime %lld min, free heap %u B (min %u B)",
                     (long long)(now / US / 60),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
            next_heap_us = now + (int64_t)AWAKE_HEAP_LOG_S * US;
        }
        if (now - last_ok_us > (int64_t)AWAKE_STALL_REBOOT_S * US) {
            /* Nothing has succeeded for half an hour while the link is up, so
             * the loop itself is wedged. A reset returns to always-on mode;
             * sitting here shows a stale frame for ever. */
            ESP_LOGE(TAG, "always-on: no successful exchange in %d s; restarting",
                     AWAKE_STALL_REBOOT_S);
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Leaving always-on: a frame still waiting on the floor goes to the glass
     * now rather than being thrown away. The next thing this device does is
     * deep-sleep, so dropping it would strand the panel on older content until
     * the next wake -- the one case where honouring the floor costs more than
     * it protects. */
    if (pending.frame) {
        ESP_LOGI(TAG, "always-on: painting held frame %s before sleeping",
                 pending.fo.etag);
        frame_paint(&pending);
    }
#if defined(BOARD_HAS_TOUCH) && defined(BOARD_OVERLAY_PARTIAL)
    sse_stop();
#endif
    wifi_sta_stop();
}

static void sleep_forever_or_until_timer(void)
{
    buzzer_idle();   /* never leave the piezo driven across a sleep (#258) */
    /* Always-on power policy: server config opted this device out of deep sleep
     * (config.always_on, read on every /status poll). Runs until config or a
     * draining cell says otherwise, then falls through to the normal sleep
     * below on sleep_interval_s -- no reboot needed either way. */
    if (rest_config_get()->always_on) always_on_loop();

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
    if (s_sleep_override_s <= 0) {
        interval = collection_next_sleep_s(interval);
        /* Synchronized wake: when /status carried an absolute wake_at,
         * convert it to a delta HERE, at sleep entry, so the fetch +
         * paint time since the response doesn't push the wake late.
         * No-op without a target; a collection-shortened interval wins. */
        interval = wake_align_sleep_s(interval);
    }
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
    /* Drift-corrected program: the RTC slow clock's measured drift (learned
     * from the per-wake Date-header disciplines) trims the timer so a long
     * aligned sleep lands on the wall-clock instant, not ppm-late. Exactly
     * interval * 1e6 until a drift estimate exists. */
    esp_sleep_enable_timer_wakeup(wake_align_timer_us(interval));
    power_latch_hold_through_sleep();   /* or the timer wakes nothing */
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- app ---------- */

static bool apply_ble_result(ble_setup_result_t result);

static bool registered_wifi_recovery_needed(void)
{
    const rest_config_t *cfg = rest_config_get();
    return !wifi_creds_present() &&
           rest_config_has_server() &&
           cfg->device_token[0];
}

static ble_setup_mode_t setup_ble_mode(void)
{
    if (registered_wifi_recovery_needed()) {
        ESP_LOGI(TAG, "saved registration found; BLE will repair Wi-Fi in place");
        return BLE_SETUP_MODE_MAINTENANCE;
    }
    return BLE_SETUP_MODE_NEW_DEVICE;
}

static void run_provisioning_then_reboot(const char *note)
{
    const char *portal_note = note;
    while (1) {
        ESP_LOGW(TAG, "opening captive portal%s%s",
                 portal_note ? ": " : "", portal_note ? portal_note : "");
        /* Bring the AP up FIRST (joinable in ~1-2 s), THEN paint the portal
         * splash. The user can use 192.168.4.1 immediately, or hold the board's
         * maintenance control to tear AP down and switch to Companion BLE. */
        provisioning_begin();
        if (portal_note) splash_show_portal_note(portal_note);
        else             splash_show_portal();
        provisioning_result_t result = provisioning_serve();
        if (result == PROVISIONING_RESULT_SAVED) {
            ESP_LOGI(TAG, "creds saved; rebooting to use them");
            esp_restart();
            /* not reached */
        }
        if (result == PROVISIONING_RESULT_BLE_REQUESTED) {
            ble_setup_result_t ble_result = ble_setup_run(
                setup_ble_mode(), BLE_SETUP_TIMEOUT_S);
            if (apply_ble_result(ble_result)) return;
            ESP_LOGW(TAG, "BLE setup ended without a saved configuration; returning to AP");
            portal_note = "Bluetooth ended; use AP";
            continue;
        }
        break;
    }

    /* Portal expired with no client ever joining (or no submission). An
     * onboarded device most likely got here through an outage (network
     * switched off overnight, router down for hours), not bad credentials:
     * sleep on a timer and retry the stored network, and on another miss the
     * portal reopens, so a user standing at the panel still gets the setup
     * screen within one cycle (dmellok/tesserae#270). Only a
     * never-onboarded device powers
     * down completely and requires a manual RESET, since there the stored
     * configuration itself is the likely problem. The board's RESET button
     * (chip EN line) causes a fresh boot which re-enters the portal. */
    if (wifi_creds_present() &&
        (rest_config_get()->device_token[0] != '\0' || relay_ready())) {
        ESP_LOGW(TAG, "captive portal expired idle; retrying stored Wi-Fi in %d min",
                 WIFI_PORTAL_RETRY_SLEEP_S / 60);
        s_sleep_override_s = WIFI_PORTAL_RETRY_SLEEP_S;
        sleep_forever_or_until_timer();
        /* not reached */
    }
    ESP_LOGW(TAG, "captive portal expired idle; deep sleep until RESET button");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    /* No wake source at all, so on a latched board drop the rail: a real
     * power-off, not an indefinite trickle. No-op elsewhere. */
    power_latch_release();
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

/* Fetch the current frame and paint it if the server returned a new one. Used
 * inside the touch-linger and button-wake windows and by the always-on loop,
 * where WiFi is kept up so repeated interactions don't pay reconnect/boot
 * latency. Any touch/button params must already be set on the REST client.
 * Returns true if a new frame was painted. */
/* Fetch + decode the current frame. True means `out` owns a frame that must
 * eventually reach frame_paint() or frame_discard(). False covers 304, 204 and
 * every error alike: nothing new, nothing allocated.
 *
 * held_etag, when non-NULL, is the etag of a frame already fetched and waiting
 * on the repaint floor. It exists because If-None-Match necessarily carries the
 * etag of what is ON THE GLASS, not what we are holding -- that is what lets a
 * held frame be superseded safely. The consequence is that every poll during a
 * deferral is a 200 for the frame we already have, and on this panel that is
 * 1.3 MB re-downloaded per poll. Comparing etags before touching the body turns
 * that back into metadata-only round trips. */
static bool frame_fetch(const char *server_url, const char *held_etag,
                        pending_frame_t *out)
{
    memset(out, 0, sizeof *out);
    if (rest_get_frame(&out->fo, 8000) != REST_OK) return false;
    if (held_etag && held_etag[0] && out->fo.etag[0] &&
        strcmp(held_etag, out->fo.etag) == 0)
        return false;   /* already holding exactly this frame */
    /* A 200 body carries the freshest button_wake_s; adopt it mid-window too. */
    if (out->fo.button_wake_s >= 0)
        rest_config_set_button_wake_s(out->fo.button_wake_s);
    char fullurl[512];
    resolve_url(server_url, out->fo.url, fullurl, sizeof fullurl);
    fetched_image_t img;
    if (image_fetch(fullurl, &img) != ESP_OK) return false;
    if (image_decode_to_frame(&img, fullurl, &out->frame) != ESP_OK)
        out->frame = NULL;
    free(img.data);
    if (!out->frame) return false;
    /* "Downloaded" hooks fire here, not at paint time: they re-anchor specs to
     * the new digest, which the overlay and touch engines need as soon as the
     * bytes exist, whether or not the floor lets us paint yet. */
    overlay_frame_downloaded(out->fo.etag);
    proto2_frame_downloaded(out->fo.etag, out->fo.manifest_digest,
                            out->fo.manifest_url);
    touch3_frame_downloaded(out->fo.etag, out->fo.layout_digest);
    return true;
}

/* Put a fetched frame on the glass and release it. Consumes `p`. */
static void frame_paint(pending_frame_t *p)
{
    ESP_ERROR_CHECK(epd_port_init());
    epd_init();
    /* v3 draws its controls INTO the frame the server left blank there, so one
     * refresh shows image + controls together (touch-v3 firmware-spec §4). */
    touch3_compose(p->frame);
    epd_display(p->frame);
    epd_sleep();
    /* The etag is stored HERE rather than at fetch time, which is what makes a
     * deferred frame safe: a frame that was fetched and then superseded before
     * it ever reached the glass must not leave its etag behind as the thing we
     * send If-None-Match for. */
    if (p->fo.etag[0]) rest_config_set_frame_etag(p->fo.etag);
    /* Keep the overlay buffers current so follow-up echoes and schema-2
     * patch applications in this same linger window composite onto the
     * frame actually on glass (frame freed only after the copy). */
    overlay_after_paint(p->frame, p->fo.etag);
    proto2_frame_painted(p->fo.etag);   /* server-wins: clears the ledger */
    touch3_after_paint(p->fo.etag);     /* note the layout on glass, reset hygiene */
    free(p->frame);
    p->frame = NULL;
}

/* Release a fetched frame without painting it (superseded while held). */
static void frame_discard(pending_frame_t *p)
{
    free(p->frame);
    p->frame = NULL;
}

static bool fetch_and_paint_current(const char *server_url)
{
    pending_frame_t p;
    if (!frame_fetch(server_url, NULL, &p)) return false;
    frame_paint(&p);
    return true;
}

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
        ESP_LOGI(TAG, "replayed queued touch (event %llu) -> %d",
                 (unsigned long long)e.event_id, fs);
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
    power_latch_hold_through_sleep();   /* the recheck timer must survive */
    esp_deep_sleep_start();
    /* not reached */
}

/* ---------- front-button factory reset ---------- */

/* Hold Refresh (Key1) for BLE_MAINTENANCE_HOLD_S and release before
 * FACTORY_RESET_HOLD_S to enter bounded BLE maintenance. Keep holding through
 * FACTORY_RESET_HOLD_S to erase NVS and reboot into setup. The initiating press is an ordinary ext1 wake
 * (or the button can be held through a RESET/power-on), so this runs on every
 * boot but costs one GPIO read when the button is already up. It must run
 * BEFORE wifi_manager_init() so the erase cannot race the connect path's own
 * NVS writes (fast-connect hints, creds). Held through the reboot, the check
 * simply re-arms: NVS is already blank, and the portal opens on release. */
#if defined(BOARD_HAS_BUTTONS) && defined(BOARD_BTN_REFRESH_PIN)
static bool maybe_factory_reset_hold(button_id_t woke_btn, bool first_boot)
{
    if (!buttons_is_maintenance_button(woke_btn) && !first_boot) return false;

    buttons_poll_init();
    vTaskDelay(pdMS_TO_TICKS(10));   /* pull-up settle before the first read */
    if (!buttons_maintenance_is_pressed()) return false;

    int held_ms = 0;
    while (buttons_maintenance_is_pressed()) {
        if (held_ms == 0) {
            ESP_LOGW(TAG,
                     "refresh held: release after %d s for BLE maintenance; keep holding %d s to reset",
                     BLE_MAINTENANCE_HOLD_S, FACTORY_RESET_HOLD_S);
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
        if (held_ms >= BLE_MAINTENANCE_HOLD_S * 1000) {
            ESP_LOGI(TAG, "released after %d ms; entering BLE maintenance", held_ms);
            return true;
        }
        ESP_LOGI(TAG, "released after %d ms; continuing as a normal press", held_ms);
    }
    return false;
}
#else
static bool maybe_factory_reset_hold(button_id_t woke_btn, bool first_boot)
{
    (void)woke_btn; (void)first_boot;
    return false;
}
#endif

static bool apply_ble_result(ble_setup_result_t result)
{
    switch (result) {
    case BLE_SETUP_RESULT_CONFIGURED:
    case BLE_SETUP_RESULT_REBOOT:
        /* The setup splash replaced the physical panel contents. Persistently
         * invalidate the cached ETag before restarting so the next ordinary
         * wake cannot leave that splash visible after a server 304. */
        rest_config_set_frame_etag("");
        ESP_ERROR_CHECK(rest_config_save());
        esp_restart();
        return true;
    case BLE_SETUP_RESULT_CLEAR_WIFI:
        ESP_ERROR_CHECK(wifi_creds_clear());
        esp_restart();
        return true;
    case BLE_SETUP_RESULT_FACTORY_RESET:
        ESP_ERROR_CHECK(nvs_flash_erase());
        esp_restart();
        return true;
    default:
        return false;
    }
}

/* Awake button windows used to dispatch Refresh on its first edge, making the
 * 3-second BLE gesture work only when waking from deep sleep. Enter through a
 * fresh software cycle after stopping Wi-Fi so every runtime state converges
 * on the same bounded maintenance path. */
static void enter_ble_maintenance_from_awake_window(void)
{
    ESP_LOGI(TAG, "awake-window Refresh hold: entering BLE maintenance");
    rest_set_button(NULL, 0);
    wifi_sta_stop();
    ble_setup_result_t result = ble_setup_run(
        BLE_SETUP_MODE_MAINTENANCE, BLE_SETUP_TIMEOUT_S);
    if (apply_ble_result(result)) return;   /* action paths restart */

    /* Timeout/error also replaced the panel with the maintenance splash.
     * Persist an empty ETag and restart so the normal cycle restores content. */
    rest_config_set_frame_etag("");
    ESP_ERROR_CHECK(rest_config_save());
    esp_restart();
}

/* One conditional relay frame poll, painting only a frame that decrypted AND
 * matches this panel's geometry. Returns true when the glass changed.
 *
 * Shared by the once-per-wake cycle and the post-button window, so both agree
 * on the rules that matter: the length is validated against bytes that passed
 * the GCM tag rather than a plaintext header the relay could have altered, and
 * the ETag is committed only after the frame actually reaches the glass. */
static bool relay_poll_and_paint(void)
{
    uint8_t *plain = NULL, *owned = NULL;
    size_t plain_len = 0;
    bool painted = false;

    switch (relay_fetch_frame(&plain, &plain_len, &owned)) {
    case RELAY_FRAME_NEW:
        if (plain_len == (size_t)EPD_BUF_BYTES) {
            ESP_LOGI(TAG, "relay frame %u bytes; painting (~30 s)...",
                     (unsigned)plain_len);
            ESP_ERROR_CHECK(epd_port_init());
            epd_init();
            epd_display(plain);
            epd_sleep();
            rest_config_set_ui_state(UI_CONNECTED);
            relay_commit_frame();   /* only now is the ETag truthful */
            painted = true;
        } else {
            ESP_LOGE(TAG, "relay frame is %u bytes, panel expects %u; "
                          "not painting",
                     (unsigned)plain_len, (unsigned)EPD_BUF_BYTES);
        }
        free(owned);
        break;
    case RELAY_FRAME_UNCHANGED:
        ESP_LOGI(TAG, "relay frame unchanged (304); keeping the image");
        break;
    case RELAY_FRAME_NONE:
        ESP_LOGI(TAG, "relay has no frame yet (204)");
        break;
    default:
        ESP_LOGW(TAG, "relay frame fetch failed");
        break;
    }
    return painted;
}

void app_main(void)
{
    /* Hold our own power on before anything else: on a latched board (Xteink
     * X4) a unit that does not self-latch stays up only while the button is
     * held, so every line below would be racing the user's thumb. No-op
     * elsewhere. See power_latch.h. */
    power_latch_hold();

    /* Park the SD card's chip-select before ANY code touches the shared SPI
     * bus (selftests included) -- a floating CS with a card fitted disturbs
     * panel refreshes. No-op on boards without a slot. See sdcard.h. */
    sdcard_quiesce();

    /* NVS up front, because the button beep below is the first thing that asks
     * rest_config_get() for an answer and it cannot get a true one otherwise:
     * the lazy load behind it opens NVS, that open fails until the partition is
     * initialised (which wifi_manager does, hundreds of lines later), and the
     * config then reads as defaults with the beep off. On a deep-sleep button
     * wake that made the press silent while touch, which happens after the
     * explicit load, sounded correctly. Idempotent and cheap; wifi_manager
     * still owns the erase-and-retry path if this fails here. */
    esp_err_t nvs_early = nvs_flash_init();
    if (nvs_early != ESP_OK)
        ESP_LOGW(TAG, "early nvs init: %s (config reads defaults until wifi init)",
                 esp_err_to_name(nvs_early));

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
        uint64_t ev = ++s_button_event_seq;
        ESP_LOGI(TAG, "woke on '%s' button: report + refresh (event %llu)",
                 button_name(woke_btn), (unsigned long long)ev);
        rest_set_button(button_name(woke_btn), ev);
        buzzer_feedback();   /* confirm the press now, not after the repaint (#258) */
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

#ifdef OVERLAY_SELFTEST
    /* Overlay bring-up (partial-refresh boards): paint a gray-band base, then
     * run a synthetic overlay spec offline -- one invert target and one slot
     * fed by a code-drawn digits atlas -- cycling values 0-9 with DU partial
     * refreshes and per-op timings on serial. No networking. */
    ESP_LOGW(TAG, "OVERLAY_SELFTEST: partial-refresh cycle (no networking)");
    {
        /* Up to 32 small targets (cap-raise verification: a grid of tiles
         * sized to the panel, 8x4 on the E1003, 2x4 on the Sticky) plus the
         * digits slot. Built at runtime to keep the literal sane. */
        const int bpp   = epd_active_driver()->info.bpp;
        const int tcols = (EPD_WIDTH - 40) / 220 > 8 ? 8 : (EPD_WIDTH - 40) / 220;
        const int trows = (EPD_HEIGHT - 140) / 140 > 4 ? 4 : (EPD_HEIGHT - 140) / 140;
        const int ntargets = tcols * trows;
        const int slot_y = EPD_HEIGHT - 60;
        char *spec_json = malloc(8192);
        int sj = snprintf(spec_json, 8192,
            "{\"schema\":1,\"frame_digest\":\"0000000000000000\",\"targets\":[");
        for (int i = 0; i < ntargets; i++) {
            sj += snprintf(spec_json + sj, 8192 - (size_t)sj,
                "%s{\"id\":\"t%d\",\"x\":%d,\"y\":%d,\"w\":180,\"h\":120,"
                "\"echo\":\"invert\"}", i ? "," : "", i,
                40 + (i % tcols) * 220, 40 + (i / tcols) * 140);
        }
        sj += snprintf(spec_json + sj, 8192 - (size_t)sj, "],"
            "\"slots\":[{\"id\":\"s1\",\"x\":140,\"y\":%d,\"w\":200,\"h\":32,"
            "\"key\":\"v\",\"align\":\"right\",\"atlas\":\"a1\"}],"
            "\"atlases\":[{\"id\":\"a1\",\"digest\":\"0000000000000000\","
            "\"height\":32,\"url\":\"-\",\"format\":\"4bpp-gray\",\"glyphs\":{"
            "\"0\":{\"x\":0,\"w\":20},\"1\":{\"x\":20,\"w\":20},"
            "\"2\":{\"x\":40,\"w\":20},\"3\":{\"x\":60,\"w\":20},"
            "\"4\":{\"x\":80,\"w\":20},\"5\":{\"x\":100,\"w\":20},"
            "\"6\":{\"x\":120,\"w\":20},\"7\":{\"x\":140,\"w\":20},"
            "\"8\":{\"x\":160,\"w\":20},\"9\":{\"x\":180,\"w\":20}}}]}", slot_y);
        (void)sj;
        overlay_spec_t *sp = malloc(sizeof *sp);
        uint8_t *base = heap_caps_malloc(EPD_BUF_BYTES, TESSERAE_FB_CAPS);
        enum { AW = 200, AH = 32, GW = 20 };
        uint8_t *atlas = heap_caps_malloc(AW / 2 * AH, TESSERAE_FB_CAPS);
        if (!sp || !base || !atlas || !spec_json ||
            !overlay_spec_parse(spec_json, (size_t)sj,
                                EPD_WIDTH, EPD_HEIGHT, sp)) {
            ESP_LOGE(TAG, "OVERLAY_SELFTEST: setup failed");
        } else {
            /* Digits atlas: 200x32 4bpp seven-segment, black on white. */
            memset(atlas, 0xFF, AW / 2 * AH);
            static const uint8_t SEG[10] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
            for (int d = 0; d < 10; d++) {
                for (int yy = 0; yy < AH; yy++) {
                    for (int xx = 2; xx < GW - 2; xx++) {
                        uint8_t sgs = SEG[d];
                        bool topH = yy < 4, midH = yy >= 14 && yy < 18, botH = yy >= AH - 4;
                        bool lftV = xx < 6, rgtV = xx >= GW - 8;
                        bool on = (topH && (sgs & 0x01)) || (botH && (sgs & 0x08)) ||
                                  (midH && (sgs & 0x40)) ||
                                  (lftV && ((yy < 16 && (sgs & 0x20)) || (yy >= 16 && (sgs & 0x10)))) ||
                                  (rgtV && ((yy < 16 && (sgs & 0x02)) || (yy >= 16 && (sgs & 0x04))));
                        if (on) {
                            int ax = d * GW + xx;
                            uint8_t *b = &atlas[(size_t)yy * (AW / 2) + ax / 2];
                            *b = (ax & 1) ? (uint8_t)(*b & 0xF0) : (uint8_t)(*b & 0x0F);
                        }
                    }
                }
            }
            sp->atlases[0].bits = atlas;

            /* Gray ramp base, top to bottom, at the panel's own depth. */
            const int pitch = EPD_BUF_BYTES / EPD_HEIGHT;
            for (int yy = 0; yy < EPD_HEIGHT; yy++) {
                uint8_t fill;
                if (bpp == 4)      { uint8_t g = (uint8_t)((yy * 16) / EPD_HEIGHT); fill = (uint8_t)((g << 4) | g); }
                else if (bpp == 2) { uint8_t g = (uint8_t)((yy * 4) / EPD_HEIGHT);  fill = (uint8_t)(g * 0x55); }
                else               { fill = ((yy * 2) / EPD_HEIGHT) ? 0xFF : 0x00; }
                memset(base + (size_t)yy * pitch, fill, pitch);
            }
            ESP_ERROR_CHECK(epd_port_init());
            epd_init();
            ESP_LOGW(TAG, "OVERLAY_SELFTEST: painting base (GC16 full)...");
            epd_display(base);

            ESP_LOGW(TAG, "OVERLAY_SELFTEST: %d targets parsed (cap %d)",
                     sp->n_targets, OVERLAY_MAX_TARGETS);

            /* Hit-test cost across 32 targets: worst case is a miss (walks
             * the whole list). Should be microseconds per lookup. */
            int64_t t0 = esp_timer_get_time();
            volatile int hits = 0;
            for (int i = 0; i < 10000; i++)
                if (overlay_hit_target(sp, (i * 37) % EPD_WIDTH,
                                       (i * 53) % EPD_HEIGHT)) hits++;
            ESP_LOGW(TAG, "OVERLAY_SELFTEST: 10000 hit-tests in %lld us (%d hits)",
                     esp_timer_get_time() - t0, hits);

            overlay_hygiene_t hy; overlay_hygiene_reset(&hy);
            /* Echo three scattered targets (first, middle, last). */
            const int tap_idx[3] = { 0, ntargets / 2, ntargets - 1 };
            for (int i = 0; i < 3; i++) {
                const int tx = 40 + (tap_idx[i] % tcols) * 220 + 10;
                const int ty = 40 + (tap_idx[i] / tcols) * 140 + 10;
                const overlay_target_t *t = overlay_hit_target(sp, tx, ty);
                if (!t) { ESP_LOGE(TAG, "OVERLAY_SELFTEST: tap %d missed!", i); continue; }
                t0 = esp_timer_get_time();
                overlay_invert_rect(base, EPD_WIDTH, EPD_HEIGHT, bpp, t->x, t->y, t->w, t->h);
                epd_display_partial(base, t->x, t->y, t->w, t->h, true);
                overlay_hygiene_tick(&hy);
                ESP_LOGW(TAG, "OVERLAY_SELFTEST: echo '%s' in %lld ms",
                         t->id, (esp_timer_get_time() - t0) / 1000);
                vTaskDelay(pdMS_TO_TICKS(400));
            }

            uint8_t *pristine = heap_caps_malloc(EPD_BUF_BYTES, TESSERAE_FB_CAPS);
            if (pristine) memcpy(pristine, base, EPD_BUF_BYTES);
            int64_t seq = -1;
            for (int d = 0; d <= 9 && pristine; d++) {
                char doc[64];
                snprintf(doc, sizeof doc, "{\"seq\":%d,\"values\":{\"v\":\"%d\"}}", d + 1, d);
                if (!overlay_values_apply(sp, doc, strlen(doc), &seq)) continue;
                overlay_slot_t *sl = &sp->slots[0];
                t0 = esp_timer_get_time();
                overlay_draw_slot(base, pristine, EPD_WIDTH, EPD_HEIGHT, bpp, sl,
                                  &sp->atlases[0]);
                bool full = overlay_hygiene_tick(&hy);
                if (full) { epd_display(base); overlay_hygiene_reset(&hy); }
                else epd_display_partial(base, sl->x, sl->y, sl->w, sl->h, true);
                ESP_LOGW(TAG, "OVERLAY_SELFTEST: slot=\"%s\" in %lld ms%s",
                         sl->value, (esp_timer_get_time() - t0) / 1000,
                         full ? " (hygiene GC16 full)" : "");
                vTaskDelay(pdMS_TO_TICKS(800));
            }
            epd_sleep();
            ESP_LOGW(TAG, "OVERLAY_SELFTEST: done; halting. Press RESET to repeat.");
        }
    }
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
#endif

#if defined(TOUCH3_SELFTEST) && defined(BOARD_HAS_TOUCH) && \
    defined(BOARD_OVERLAY_PARTIAL)
    /* Touch-v3 bring-up (E1003): render all four primitives from a synthetic
     * spec onto a blank frame, then exercise each interaction offline with DU
     * partial refreshes and per-op timings on serial. This is the on-hardware
     * validator for the geometry + feedback paths while the server's Phase-1
     * endpoints (/frame/spec, /interact, /frame/stream) do not exist yet, so
     * there is nothing to fetch. No networking.
     *
     * What to check on the glass, against the server's canvas preview:
     *   - button: rounded frame, 2 px stroke, radius 12; press inverts the rect
     *   - switch: pill track, thumb parked left (off) / right (on), mid fill on
     *   - slider: 8 px track inset 20, 36 px paper thumb, mid active fill,
     *             value_text tracking the thumb
     *   - stepper: rounded frame, soft dividers at the thirds, minus bar and
     *              plus cross, value centred
     * Latency target is <= ~500 ms per DU rect (firmware-spec §12). */
    ESP_LOGW(TAG, "TOUCH3_SELFTEST: primitive render + feedback (no networking)");
    {
        /* One atlas: digits + '%' at 28/700, the contract's "value" role. The
         * strip is INK scale (0 = paper .. 15 = ink) like a real v3 atlas, NOT
         * the panel's white-scale -- t3_draw_text inverts on the way out. */
        enum { AW = 220, AH = 32, GW = 20 };
        char *spec_json = malloc(4096);
        t3_spec_t *sp = malloc(sizeof *sp);
        uint8_t *base = heap_caps_malloc(EPD_BUF_BYTES,
                                         TESSERAE_FB_CAPS);
        uint8_t *atlas = heap_caps_malloc(AW / 2 * AH,
                                          TESSERAE_FB_CAPS);
        int sj = 0;
        if (spec_json)
            sj = snprintf(spec_json, 4096,
              "{\"layout_digest\":\"5e1f7e57\","
              "\"atlases\":[{\"id\":\"v28\",\"digest\":\"selftest\","
              " \"url\":\"-\",\"format\":\"gray4\",\"px\":28,\"weight\":700,"
              " \"strip_w\":%d,\"strip_h\":%d,\"ascent\":26,\"descent\":6,"
              " \"space_adv\":10,\"glyphs\":{"
              "  \"0\":{\"x\":0,\"w\":18,\"adv\":20},"
              "  \"1\":{\"x\":20,\"w\":18,\"adv\":20},"
              "  \"2\":{\"x\":40,\"w\":18,\"adv\":20},"
              "  \"3\":{\"x\":60,\"w\":18,\"adv\":20},"
              "  \"4\":{\"x\":80,\"w\":18,\"adv\":20},"
              "  \"5\":{\"x\":100,\"w\":18,\"adv\":20},"
              "  \"6\":{\"x\":120,\"w\":18,\"adv\":20},"
              "  \"7\":{\"x\":140,\"w\":18,\"adv\":20},"
              "  \"8\":{\"x\":160,\"w\":18,\"adv\":20},"
              "  \"9\":{\"x\":180,\"w\":18,\"adv\":20},"
              "  \"%%\":{\"x\":200,\"w\":18,\"adv\":20}}}],"
              "\"primitives\":["
              /* Icon + label exercises the bundled Phosphor path; with no font
               * built in it degrades to label-only, which is also worth
               * seeing on the glass. */
              " {\"id\":\"btn_scene\",\"type\":\"button\","
              "  \"rect\":{\"x\":120,\"y\":160,\"w\":320,\"h\":120},"
              "  \"icon\":{\"name\":\"lightbulb\",\"weight\":\"bold\",\"px\":48},"
              "  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
              " {\"id\":\"sw_desk\",\"type\":\"switch\","
              "  \"rect\":{\"x\":520,\"y\":160,\"w\":320,\"h\":120},"
              "  \"value_key\":\"ha:light.desk\",\"state\":\"off\","
              "  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
              " {\"id\":\"sl_bri\",\"type\":\"slider\","
              "  \"rect\":{\"x\":120,\"y\":360,\"w\":720,\"h\":140},"
              "  \"axis\":\"x\",\"min\":0,\"max\":100,\"step\":5,\"value\":40,"
              "  \"value_text\":{\"atlas\":\"v28\",\"align\":\"center\","
              "                  \"suffix\":\"%%\",\"max_chars\":4},"
              "  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
              " {\"id\":\"st_vol\",\"type\":\"stepper\","
              "  \"rect\":{\"x\":120,\"y\":560,\"w\":360,\"h\":120},"
              "  \"min\":0,\"max\":30,\"step\":1,\"value\":12,"
              "  \"value_text\":{\"atlas\":\"v28\",\"align\":\"center\","
              "                  \"max_chars\":2},"
              "  \"action\":{\"tier\":1,\"type\":\"ha\"}}]}",
              AW, AH);

        if (!spec_json || !sp || !base || !atlas ||
            !t3_spec_parse(spec_json, (size_t)sj, EPD_WIDTH, EPD_HEIGHT, sp)) {
            ESP_LOGE(TAG, "TOUCH3_SELFTEST: setup failed");
        } else {
            /* Seven-segment digits + a '%' block, drawn in INK scale. */
            memset(atlas, 0x00, AW / 2 * AH);          /* 0x0 = paper */
            static const uint8_t SEG[10] = {0x3F,0x06,0x5B,0x4F,0x66,
                                            0x6D,0x7D,0x07,0x7F,0x6F};
            for (int d = 0; d < 11; d++) {
                for (int yy = 0; yy < AH; yy++) {
                    for (int xx = 2; xx < GW - 2; xx++) {
                        bool on;
                        if (d == 10) {                  /* '%': two blobs + bar */
                            on = (yy < 10 && xx < 8) || (yy >= AH - 10 && xx >= GW - 10) ||
                                 (xx + yy > GW - 4 && xx + yy < GW + 2);
                        } else {
                            uint8_t sgs = SEG[d];
                            bool topH = yy < 4, midH = yy >= 14 && yy < 18;
                            bool botH = yy >= AH - 4;
                            bool lftV = xx < 6, rgtV = xx >= GW - 8;
                            on = (topH && (sgs & 0x01)) || (botH && (sgs & 0x08)) ||
                                 (midH && (sgs & 0x40)) ||
                                 (lftV && ((yy < 16 && (sgs & 0x20)) ||
                                           (yy >= 16 && (sgs & 0x10)))) ||
                                 (rgtV && ((yy < 16 && (sgs & 0x02)) ||
                                           (yy >= 16 && (sgs & 0x04))));
                        }
                        if (!on) continue;
                        int ax = d * GW + xx;
                        if (ax >= AW) continue;
                        uint8_t *b = &atlas[(size_t)yy * (AW / 2) + ax / 2];
                        /* 0xF = full ink in the atlas's own scale. */
                        *b = (ax & 1) ? (uint8_t)(*b | 0x0F)
                                      : (uint8_t)(*b | 0xF0);
                    }
                }
            }
            sp->atlases[0].bits = atlas;

            /* Blank paper frame: this stands in for the server's image with
             * the control rects left blank. Panel scale, so 0xF = white. */
            memset(base, 0xFF, EPD_BUF_BYTES);

            ESP_ERROR_CHECK(epd_port_init());
            epd_init();
            int64_t t0 = esp_timer_get_time();
            for (int i = 0; i < sp->n_prims; i++)
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp,
                                  &sp->prims[i]);
            ESP_LOGW(TAG, "TOUCH3_SELFTEST: composed %d primitives in %lld us",
                     sp->n_prims, esp_timer_get_time() - t0);
            {   /* Report the icon path's state explicitly: a missing font is a
                 * silent label-only degrade otherwise. */
                t3_icon_ref_t probe = { .present = true, .px = 48 };
                snprintf(probe.name, sizeof probe.name, "lightbulb");
                int iw = t3_icon_width(&probe);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: icon 'lightbulb' -> %s",
                         iw ? "rendered from bundled Phosphor"
                            : "NO FONT (buttons are label-only)");
            }
            ESP_LOGW(TAG, "TOUCH3_SELFTEST: painting base (GC16 full)...");
            epd_display(base);

            /* Hit-test cost: a miss walks the whole primitive list. */
            t0 = esp_timer_get_time();
            volatile int hits = 0;
            for (int i = 0; i < 10000; i++)
                if (t3_hit(sp, (i * 37) % EPD_WIDTH,
                           (i * 53) % EPD_HEIGHT) >= 0) hits++;
            ESP_LOGW(TAG, "TOUCH3_SELFTEST: 10000 hit-tests in %lld us (%d hits)",
                     esp_timer_get_time() - t0, hits);

            /* 0. Waveform sweep. Mode numbers index the waveform table in the
             * PANEL's flash, so which index is A2 is a hardware fact, not a
             * constant -- measure rather than assume (DU's real 1.07 s already
             * disproved this driver's "~300 ms" comment). Same rect each time,
             * inverted between passes so every mode has real work to do. */
            {
                extern int it8951_bench_wave(const uint8_t *, int, int, int,
                                             int, int);
                t3_prim_t *bench = t3_prim_by_id(sp, "sw_desk");
                t3_rect_t br = t3_feedback_rect(sp, bench);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: waveform sweep on %dx%d rect",
                         br.w, br.h);
                for (int mode = 1; mode <= 7; mode++) {
                    overlay_invert_rect(base, EPD_WIDTH, EPD_HEIGHT, 4,
                                        br.x, br.y, br.w, br.h);
                    int ms = it8951_bench_wave(base, br.x, br.y, br.w, br.h,
                                               mode);
                    ESP_LOGW(TAG, "TOUCH3_SELFTEST:   mode %d -> %d ms", mode,
                             ms);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                /* Leave the rect as it started (7 inversions = inverted). */
                overlay_invert_rect(base, EPD_WIDTH, EPD_HEIGHT, 4,
                                    br.x, br.y, br.w, br.h);
                epd_display(base);
            }

            /* 1. Button press: invert the rect, then restore. */
            t3_prim_t *bp = t3_prim_by_id(sp, "btn_scene");
            for (int i = 0; i < 2; i++) {
                t0 = esp_timer_get_time();
                if (i == 0)
                    overlay_invert_rect(base, EPD_WIDTH, EPD_HEIGHT, 4,
                                        bp->rect.x, bp->rect.y,
                                        bp->rect.w, bp->rect.h);
                else
                    t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, bp);
                epd_display_partial_mode(base, bp->rect.x, bp->rect.y,
                                         bp->rect.w, bp->rect.h, EPD_RF_DU);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: button %s in %lld ms",
                         i == 0 ? "press" : "release",
                         (esp_timer_get_time() - t0) / 1000);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            /* 2. Switch: toggle twice, refreshing only the track. */
            t3_prim_t *wp = t3_prim_by_id(sp, "sw_desk");
            for (int i = 0; i < 2; i++) {
                wp->state = !wp->state;
                t3_rect_t tr = t3_feedback_rect(sp, wp);
                t0 = esp_timer_get_time();
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, wp);
                epd_display_partial_mode(base, tr.x, tr.y, tr.w, tr.h,
                                         EPD_RF_DU);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: switch -> %s in %lld ms "
                              "(track %dx%d)", wp->state ? "on" : "off",
                         (esp_timer_get_time() - t0) / 1000, tr.w, tr.h);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            /* 3. Slider: sweep by simulating drag points across the track,
             * verifying the value math and the live thumb + value_text repaint. */
            t3_prim_t *slp = t3_prim_by_id(sp, "sl_bri");
            int origin = 0, len = 0;
            t3_slider_track(slp, &origin, &len);
            for (int i = 0; i <= 4; i++) {
                int fx = origin + (len * i) / 4;
                float v = t3_slider_value_at(slp, fx, slp->rect.y + slp->rect.h / 2);
                slp->value = v;
                t3_rect_t fr = t3_feedback_rect(sp, slp);
                t0 = esp_timer_get_time();
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, slp);
                epd_display_partial_mode(base, fr.x, fr.y, fr.w, fr.h,
                                         EPD_RF_DU);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: slider x=%d -> %d%% in %lld ms",
                         fx, (int)v, (esp_timer_get_time() - t0) / 1000);
                vTaskDelay(pdMS_TO_TICKS(400));
            }

            /* 4. Stepper: plus three times, then minus once. */
            t3_prim_t *stp = t3_prim_by_id(sp, "st_vol");
            for (int i = 0; i < 4; i++) {
                int zone = (i < 3) ? 1 : -1;
                stp->value = t3_snap(stp, stp->value +
                                     (zone > 0 ? stp->vstep : -stp->vstep));
                t0 = esp_timer_get_time();
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, stp);
                epd_display_partial_mode(base, stp->rect.x, stp->rect.y,
                                         stp->rect.w, stp->rect.h, EPD_RF_DU);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: stepper %s -> %d in %lld ms",
                         zone > 0 ? "+" : "-", (int)stp->value,
                         (esp_timer_get_time() - t0) / 1000);
                vTaskDelay(pdMS_TO_TICKS(400));
            }

            /* 4b. Waveform A/B. The timing sweep above cannot answer the two
             * questions that actually decide which waveform ships, so judge
             * them by eye here:
             *
             *   1. Does the switch's ON track render as GRAY? primitives.json
             *      fills it with `mid` (level 8 of 15). A 2-LEVEL waveform --
             *      both DU and A2 -- has no middle level and thresholds it to
             *      solid black, which also swallows the ink thumb sitting on
             *      top. Only a grayscale waveform can show it as intended.
             *   2. How badly does it ghost over repeated toggles?
             *
             * Mode 1 (DU) runs first as the baseline you already know. Each
             * candidate starts from a GC16 full clear so the previous mode's
             * ghosting cannot contaminate the next one's verdict, and the
             * STEPPER displays the mode number currently under test.
             *
             * OPT-IN (-DTOUCH3_WAVEFORM_AB): it costs ~110 s before the
             * interactive phase, which is pure friction now that mode 5 is
             * settled. Turn it back on when bringing up a new panel, whose
             * waveform table will differ again. */
#ifdef TOUCH3_WAVEFORM_AB
            {
                extern int it8951_bench_wave(const uint8_t *, int, int, int,
                                             int, int);
                static const int CAND[] = { 1, 3, 5, 7 };
                t3_prim_t *wp2 = t3_prim_by_id(sp, "sw_desk");
                t3_prim_t *sp2 = t3_prim_by_id(sp, "st_vol");
                t3_rect_t wr = t3_feedback_rect(sp, wp2);

                for (unsigned c = 0; c < sizeof CAND / sizeof CAND[0]; c++) {
                    int m = CAND[c];
                    /* Clean slate + label the panel with the mode number. */
                    wp2->state = false;
                    t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, wp2);
                    sp2->value = (float)m;
                    t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, sp2);
                    epd_display(base);
                    ESP_LOGW(TAG, "TOUCH3_SELFTEST: === waveform %d (stepper "
                                  "shows %d) ===", m, m);
                    vTaskDelay(pdMS_TO_TICKS(1200));

                    /* Toggle repeatedly so ghosting has a chance to build. */
                    for (int i = 0; i < 6; i++) {
                        wp2->state = !wp2->state;
                        t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp,
                                          wp2);
                        int ms = it8951_bench_wave(base, wr.x, wr.y, wr.w,
                                                   wr.h, m);
                        ESP_LOGW(TAG, "TOUCH3_SELFTEST:   mode %d toggle %d "
                                      "-> %s in %d ms", m, i + 1,
                                 wp2->state ? "on" : "off", ms);
                        vTaskDelay(pdMS_TO_TICKS(600));
                    }

                    /* Leave it ON and hold, so the mid-gray track fill is on
                     * the glass to be judged. */
                    wp2->state = true;
                    t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, wp2);
                    it8951_bench_wave(base, wr.x, wr.y, wr.w, wr.h, m);
                    ESP_LOGW(TAG, "TOUCH3_SELFTEST:   mode %d: LOOK NOW -- is "
                                  "the ON track grey (not solid black), and is "
                                  "the thumb visible? ghosting?", m);
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
                /* Restore a clean panel for the interactive phase. */
                wp2->state = false;
                sp2->value = 14;
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, wp2);
                t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, sp2);
                epd_display(base);
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: waveform A/B done");
            }
#endif /* TOUCH3_WAVEFORM_AB */

            /* 5. INTERACTIVE: drive the controls with a real finger. Runs the
             * same engine calls the production tier engine uses (hit-test ->
             * classify -> mutate -> redraw -> A2 partial), just without the
             * server report, so the controls behave exactly as they will in
             * the field and the whole thing works offline. Also confirms
             * orientation for free: a tap must change the control you touched
             * (firmware-spec §2 -- rects are already in framebuffer space).
             *
             * Loops until RESET rather than timing out, so the panel stays
             * usable for as long as you want to poke at it. */
            if (touch_init() == ESP_OK) {
                ESP_LOGW(TAG, "TOUCH3_SELFTEST: INTERACTIVE -- tap the button, "
                              "switch and stepper zones, drag the slider. "
                              "Press RESET to exit.");
                int partials = 0;
                for (;;) {
                    if (!touch_int_asserted()) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }
                    touch_stroke_t st;
                    touch_capture_stroke(&st, TOUCH_FIRST_POINT_MS,
                                         TOUCH_CAP_MS);
                    if (!st.valid) continue;

                    int idx = t3_hit(sp, st.x0, st.y0);
                    if (idx < 0) {
                        ESP_LOGW(TAG, "TOUCH3_SELFTEST: tap (%d,%d) -> (miss)",
                                 st.x0, st.y0);
                        continue;
                    }
                    t3_prim_t *p = &sp->prims[idx];
                    t3_rect_t fr = t3_feedback_rect(sp, p);
                    char what[32] = "";
                    int64_t tt = esp_timer_get_time();

                    switch (p->type) {
                    case T3_BUTTON:
                        /* Momentary: invert, hold so the press registers, then
                         * restore. The hold is 400 ms rather than a token
                         * 120 ms because production has a /interact round trip
                         * in the same gap -- this keeps the two paths' timing
                         * comparable, and gives the panel time to settle before
                         * the same rect is refreshed again. */
                        overlay_invert_rect(base, EPD_WIDTH, EPD_HEIGHT, 4,
                                            p->rect.x, p->rect.y,
                                            p->rect.w, p->rect.h);
                        epd_display_partial_mode(base, p->rect.x, p->rect.y,
                                                 p->rect.w, p->rect.h,
                                                 EPD_RF_DU);
                        vTaskDelay(pdMS_TO_TICKS(400));
                        t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, p);
                        snprintf(what, sizeof what, "pressed");
                        partials++;   /* the invert above is its own partial;
                                       * the restore below counts separately */
                        break;
                    case T3_SWITCH:
                        p->state = !p->state;
                        t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, p);
                        snprintf(what, sizeof what, "-> %s",
                                 p->state ? "on" : "off");
                        break;
                    case T3_SLIDER:
                        /* Settle on the LIFT point, so a drag lands where the
                         * finger left the glass. */
                        p->value = t3_slider_value_at(p, st.x1, st.y1);
                        t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, p);
                        snprintf(what, sizeof what, "-> %d", (int)p->value);
                        break;
                    case T3_STEPPER: {
                        int zone = t3_stepper_zone(p, st.x0, st.y0);
                        if (zone == 0) {      /* centre third is inert */
                            ESP_LOGW(TAG, "TOUCH3_SELFTEST: '%s' value zone "
                                          "(no change)", p->id);
                            continue;
                        }
                        p->value = t3_snap(p, p->value + (zone > 0 ? p->vstep
                                                                  : -p->vstep));
                        t3_draw_primitive(base, EPD_WIDTH, EPD_HEIGHT, 4, sp, p);
                        snprintf(what, sizeof what, "%s -> %d",
                                 zone > 0 ? "+" : "-", (int)p->value);
                        break;
                    }
                    }

                    /* Ghosting hygiene, same rule as the tier engine: a full
                     * GC16 every T3_HYGIENE_N partials. A2 ghosts more than
                     * DU, so this is what keeps the panel clean -- watch for
                     * the flash and judge whether N=8 is right. */
                    if (++partials >= T3_HYGIENE_N) {
                        partials = 0;
                        ESP_LOGW(TAG, "TOUCH3_SELFTEST: hygiene GC16");
                        epd_display(base);
                    } else {
                        epd_display_partial_mode(base, fr.x, fr.y, fr.w, fr.h,
                                                 EPD_RF_DU);
                    }
                    ESP_LOGW(TAG, "TOUCH3_SELFTEST: %s '%s' %s in %lld ms",
                             t3_ptype_name(p->type), p->id, what,
                             (esp_timer_get_time() - tt) / 1000);
                }
            }

            epd_sleep();
            ESP_LOGW(TAG, "TOUCH3_SELFTEST: done; halting. Press RESET to repeat.");
        }
    }
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
        {
            /* Raw block-layer benchmark (bypasses FATFS): 64 sectors (32 KB)
             * written + read at a scratch LBA far past the filesystem head.
             * Separates bus/card behaviour from filesystem overhead. */
            sdmmc_card_t *card = (sdmmc_card_t *)sdcard_handle();
            uint8_t *sec = heap_caps_malloc(64 * 512, MALLOC_CAP_DMA);
            if (card && sec) {
                for (int i = 0; i < 64 * 512; i++) sec[i] = (uint8_t)(i * 7);
                int64_t rb0 = esp_timer_get_time();
                esp_err_t we = sdmmc_write_sectors(card, sec, 4000000, 64);
                int64_t rb1 = esp_timer_get_time();
                esp_err_t re = sdmmc_read_sectors(card, sec, 4000000, 64);
                int64_t rb2 = esp_timer_get_time();
                ESP_LOGW(TAG, "DECK_SD_SELFTEST: raw 32KB write=%lldms (%s) read=%lldms (%s)",
                         (rb1 - rb0) / 1000, esp_err_to_name(we),
                         (rb2 - rb1) / 1000, esp_err_to_name(re));
                int64_t rb3 = esp_timer_get_time();
                for (int i = 0; i < 8; i++)
                    sdmmc_write_sectors(card, sec, 4000100 + i, 1);
                int64_t rb4 = esp_timer_get_time();
                ESP_LOGW(TAG, "DECK_SD_SELFTEST: raw 8x single-sector writes=%lldms total",
                         (rb4 - rb3) / 1000);
                free(sec);
            }
        }
        size_t n = EPD_BUF_BYTES;
        uint8_t *buf = heap_caps_malloc(n, TESSERAE_FB_CAPS);
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
#ifdef EPD_PIN_VCC_EN
            /* E1003: repeat one pass with the IT8951 rails CUT -- the state
             * mid-wake after a panel sleep, when deck/overlay code may still
             * write to the card. Tells us whether the unpowered controller's
             * bus clamp breaks SD at this clock. */
            gpio_set_level((gpio_num_t)EPD_PIN_EN, 0);
            gpio_set_level((gpio_num_t)EPD_PIN_VCC_EN, 0);
            vTaskDelay(pdMS_TO_TICKS(50));
            bool w2 = deck_cache_write_frame("sdtest2", digest, buf, n, (uint32_t)n);
            uint8_t *back2 = NULL;
            bool r2 = w2 && deck_cache_read_frame("sdtest2", digest, (uint32_t)n, &back2);
            ESP_LOGW(TAG, "DECK_SD_SELFTEST: rails-OFF pass %s",
                     (w2 && r2) ? "OK" : "FAILED");
            free(back2);
            deck_cache_delete("sdtest2", digest);
            gpio_set_level((gpio_num_t)EPD_PIN_EN, 1);
            gpio_set_level((gpio_num_t)EPD_PIN_VCC_EN, 1);
#endif
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
    bool maintenance_requested = maybe_factory_reset_hold(woke_btn, first_boot);

    ESP_ERROR_CHECK(wifi_manager_init());
    rest_config_load();

    if (maintenance_requested) {
        ble_setup_result_t result = ble_setup_run(
            BLE_SETUP_MODE_MAINTENANCE, BLE_SETUP_TIMEOUT_S);
        if (apply_ble_result(result)) return;
        /* The maintenance screen replaced the last frame. Force the ordinary
         * cycle below to restore display content even when its ETag is unchanged. */
        rest_config_set_frame_etag("");
    }

#if defined(BOARD_BTN_REFRESH_PIN)
    /* Clear Wi-Fi is an authenticated Companion action, so a display that
     * still has its server registration should return to the app first. This
     * is distinct from a new/factory-reset display, whose missing registration
     * keeps the established AP-first onboarding path below. */
    if (registered_wifi_recovery_needed() && !maintenance_requested) {
        ESP_LOGI(TAG, "registered display has no Wi-Fi; opening BLE recovery first");
        ble_setup_result_t result = ble_setup_run(
            BLE_SETUP_MODE_MAINTENANCE, BLE_SETUP_TIMEOUT_S);
        if (apply_ble_result(result)) return;
        ESP_LOGW(TAG, "BLE recovery timed out or failed; falling back to captive portal");
    }
#endif

    /* Recovered from the battery goodbye this wake: the goodbye is still on
     * the panel, so drop the cached ETag -- the server must send a full frame
     * (a 304 would leave the goodbye up). */
    if (s_battery_recovered) rest_config_set_frame_etag("");

    /* Deck cache boot: probe the SD card, advertise the capability, restore
     * nav state. Everything degrades to a no-op without a card. */
    deck_boot();
    collection_boot();

    /* An Album-only timer wake can paint a due cached photo and return to
     * sleep without starting WiFi. Manual/button/touch wakes always continue
     * through the normal server cycle. */
    if (!rest_config_get()->always_on && collection_try_local_cycle()) {
        ESP_LOGI(TAG, "offline Album wake complete; back to sleep radio-off");
        sleep_forever_or_until_timer();
        return;   /* not reached */
    }

    /* Overlay boot: restore the SD-cached overlay spec for the displayed
     * frame so a wake tap can echo before any network round trip. */
    overlay_boot();
    proto2_boot();     /* manifest for the frame on glass, from SD */
    touch3_boot();     /* v3 spec + atlases for the layout on glass, from SD */

    /* Local deck nav: a button wake whose press matches a cached link on the
     * current page paints from SD (1-2 s) and never brings the radio up. Any
     * miss keeps today's exact network behaviour -- the press's report is
     * still pending from above. */
    if (woke_by_button) {
        bool nav_fallthrough = false;
        bool nav_maintenance = false;
        if (deck_try_button(button_name(woke_btn), &s_button_event_seq,
                            &nav_fallthrough, &nav_maintenance)) {
            if (nav_maintenance) {
                enter_ble_maintenance_from_awake_window();
                return;   /* not reached */
            }
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
    uint64_t       touch_ev = 0;
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

            touch_capture_stroke_cb(&touch_st, TOUCH_FIRST_POINT_MS,
                                    TOUCH_CAP_MS, touch3_stroke_sample, NULL);
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
                ESP_LOGI(TAG, "touch (%d,%d)->(%d,%d) %ums",
                         touch_st.x0, touch_st.y0, touch_st.x1, touch_st.y1,
                         (unsigned)touch_st.ms);
                /* Audible confirmation, before anything else happens
                 * (#258): this wake still has a dispatch, a render and an
                 * e-ink flash ahead of it, which is precisely the wait the
                 * beep exists to cover. */
                buzzer_feedback();
                /* Touch v3 first: with a spec held for the layout on glass the
                 * device drew the controls and owns hit-testing + feedback;
                 * the /interact report queues until the radio is up. Then
                 * protocol v2 (interaction manifest), then today's v1 chain --
                 * each falls through only when the newer one holds nothing for
                 * the frame on glass. */
                bool p2_poll = false;
                if (touch3_try_touch(touch_st.x0, touch_st.y0,
                                     touch_st.x1, touch_st.y1,
                                     touch_st.ms, &p2_poll)) {
                    /* wake continues: frame GET + status deliver the result */
                } else if (proto2_try_touch(touch_st.x0, touch_st.y0,
                                     touch_st.x1, touch_st.y1,
                                     touch_st.ms, &p2_poll)) {
                    /* wake continues: frame GET + status deliver the result */
                } else {
                /* Overlay echo FIRST (sub-second feedback): if the tap hits a
                 * server-declared target rect, invert + partial-refresh it
                 * immediately. Never delays or replaces the dispatch below. */
                overlay_try_echo(touch_st.x1, touch_st.y1);
                /* Local deck nav: a tap landing in a cached link zone on the
                 * current page paints from SD and never reaches the server
                 * (the release point decides the hit). Misses fall through to
                 * today's dispatch below. */
                if (deck_try_touch(touch_st.x1, touch_st.y1)) {
                    ESP_LOGI(TAG, "touch served locally from deck cache; back to sleep");
                    sleep_forever_or_until_timer();
                    return;   /* not reached */
                }
                touch_ev = ++s_button_event_seq;   /* shares the wake-event counter */
                rest_set_touch(touch_st.x0, touch_st.y0, touch_st.x1, touch_st.y1,
                               touch_st.ms, rest_config_get()->last_frame_etag, touch_ev);
                }
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

    /* The captive portal collects WiFi plus ONE transport, so a device missing
     * either is not yet usable -- send it to provisioning.
     *
     * "One transport" is the important word: a relay panel deliberately has NO
     * home server URL (the portal clears it, so the panel never burns a wake
     * timing out on a server it cannot see). Gating purely on has_server()
     * therefore bounced every relay-only panel straight back to the portal on
     * each boot, and the relay cycle further down was unreachable -- pairing
     * could never complete no matter how many times the code was entered.
     * Mirrors the condition guarding that cycle. */
    const rest_config_t *bc = rest_config_get();
    bool relay_configured = bc->relay_url[0] &&
                            (relay_ready() || relay_pairing_pending());
    if (!have_creds || (!rest_config_has_server() && !relay_configured)) {
        if (have_creds) {
            ESP_LOGW(TAG, "wifi set but no Tesserae server URL and no relay "
                          "pairing; opening portal");
            run_provisioning_then_reboot("Add your Tesserae server URL, or pair "
                                         "this panel with the cloud relay");
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
        /* A device that has already onboarded (holds a home-server token, or
         * is relay-paired) almost always fails here because of a transient
         * outage -- a router reboot or being briefly out of range -- not bad
         * credentials. Don't drop a working device into AP mode on the first
         * miss (that strands it off-network and burns the battery on an
         * always-on radio). Retry over a few short-sleep wakes; only after
         * WIFI_FAIL_AP_THRESHOLD consecutive misses reopen the portal. The
         * count clears ONLY on a successful connect, never here: once past
         * the threshold, each portal-retry-sleep wake (see
         * run_provisioning_then_reboot) makes one attempt and drops straight
         * back to the portal rather than re-running the whole ladder. A
         * never-onboarded device goes straight to the portal, since there
         * the creds themselves are the likely problem. */
        bool onboarded = rest_config_get()->device_token[0] != '\0' ||
                         relay_ready();
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
    bool collection_sync_needed = false;
    char collection_id[FC_ID_CAP] = {0};
    char collection_kind[FC_KIND_CAP] = {0};
    char collection_srv_ver[FC_VERSION_CAP] = {0};
#if BOARD_OVERLAY_PARTIAL
    /* Status-borne patch doc, held until AFTER the paint: on a wake that
     * downloads a frame, the overlay buffers only exist once
     * overlay_after_paint has run -- ingesting at status time would hit the
     * no-buffer fallback and force a useless full re-download (the render
     * bin under a stable digest stays the ORIGINAL state; the patch is the
     * only carrier of the post-action pixels). Static: 1.5 KB on the main
     * task stack overflowed it (bench 2026-07-25); one wake per boot, so
     * bss zero-init gives the same per-wake semantics. */
    static char pending_patches[1536];
#endif

    /* 0. Cloud relay (docs/relay/contract.md). A remote panel never reaches the
     * home instance: it pairs through a Worker, then polls a per-device mailbox
     * and decrypts frames locally. That makes this a COMPLETE alternative to
     * everything below -- no discover/register (the token is minted by home and
     * handed over at pairing), and no overlay / deck / touch-spec features,
     * which are all home-server surfaces the relay does not proxy. So it runs
     * its own small cycle and goes straight to sleep. */
    if (c->relay_url[0] && (relay_ready() || relay_pairing_pending())) {
        if (relay_pairing_pending()) {
            switch (relay_pair_step()) {
            case RELAY_PAIR_DONE:
                ESP_LOGI(TAG, "relay pairing complete");
                splash_show_message("Paired", "Waiting for the first frame");
                break;
            case RELAY_PAIR_WAITING:
                /* Home completes pairing on ITS poll cadence, so this normally
                 * takes a few wakes. Nothing to paint; retry next wake. */
                ESP_LOGI(TAG, "relay pairing pending; retrying next wake");
                wifi_sta_stop();
                sleep_forever_or_until_timer();
                return;
            case RELAY_PAIR_EXPIRED:
                /* Single-use code, already cleared from NVS. Reopen the portal
                 * so the operator can paste a fresh one rather than leaving the
                 * panel silently dark forever. */
                wifi_sta_stop();
                run_provisioning_then_reboot("Relay pairing code expired");
                return;   /* not reached */
            default:
                ESP_LOGW(TAG, "relay pairing step failed; retrying next wake");
                wifi_sta_stop();
                sleep_forever_or_until_timer();
                return;
            }
            c = rest_config_get();
        }

        if (relay_ready()) {
            /* A press is now DELIVERED (it rides the status body below) rather
             * than merely forcing a local repaint, so the cached ETag stays:
             * home renders the press and publishes a NEW frame, which the
             * window below picks up as a 200. Clearing it here would instead
             * re-download and repaint the CURRENT image -- ~30 s of e-paper
             * that would consume most of the button window before the real
             * answer arrived. Without buttons compiled in there is nothing to
             * deliver, so keep the old force-repaint behaviour. */
#ifdef BOARD_HAS_BUTTONS
            if (woke_by_button && rest_config_get()->button_wake_s <= 0)
                rest_config_set_relay_etag("");
#else
            if (woke_by_button) rest_config_set_relay_etag("");
#endif

            relay_poll_and_paint();

            /* Telemetry on the same wake, so the Devices UI shows battery /
             * signal / firmware / last-seen for a panel it can never reach. */
            char ip[16] = {0};
            wifi_manager_get_sta_ip(ip, sizeof ip);
            relay_post_status(current_rssi(), ip, pw, ph, FW_VERSION);

            /* Adopt any settings edited on the home instance. A local device
             * picks these up from its next /status poll; a relay panel has no
             * such channel, so without this it would keep its pairing-time
             * sleep interval and button window forever. Ordered after the
             * status post because that response advertises the config etag,
             * which lets an unchanged config cost no request at all. The new
             * sleep interval applies to THIS wake's sleep below. */
            switch (relay_sync_config()) {
            case RELAY_CFG_APPLIED:
                break;                  /* already logged with the values */
            case RELAY_CFG_NONE:
                ESP_LOGI(TAG, "relay has no config published yet (204)");
                break;
            case RELAY_CFG_ERROR:
                /* Non-fatal by contract: keep the last-known config and try
                 * again next wake. */
                ESP_LOGW(TAG, "relay config sync failed; keeping current config");
                break;
            default:
                break;                  /* unchanged: nothing worth a line */
            }

#ifdef BOARD_HAS_BUTTONS
            /* Post-button window over the relay. Delivery is store-and-forward:
             * home only learns of the press on its next relay poll (up to ~30 s
             * for the first, then ~3 s for about a minute), renders, and uploads
             * the frame. So the panel must STAY AWAKE and keep polling, or the
             * answer to a press would not land until the next timer wake.
             *
             * The press is NOT resent just because no frame arrived yet -- it is
             * already sitting in the latest-only status slot, and every status
             * post this wake repeats it verbatim (see relay.c). Only a genuinely
             * new press bumps the event id. */
            if (woke_by_button && rest_config_get()->button_wake_s > 0 &&
                !relay_pairing_revoked()) {
                int32_t win_s = rest_config_get()->button_wake_s;
                ESP_LOGI(TAG, "relay button window: up to %ld s awake, polling "
                              "for the rendered frame", (long)win_s);
                buttons_poll_init();
                int64_t hard_cap = esp_timer_get_time() +
                                   BUTTON_WINDOW_CAP_S * 1000000LL;
                int64_t deadline = esp_timer_get_time() +
                                   (int64_t)win_s * 1000000;
                int64_t next_poll = 0;
                while (esp_timer_get_time() < deadline &&
                       esp_timer_get_time() < hard_cap &&
                       !relay_pairing_revoked()) {
                    button_id_t b = buttons_poll_pressed();
                    if (b != BTN_NONE) {
                        if (buttons_is_maintenance_button(b)) {
                            if (buttons_maintenance_held_for_activation()) {
                                enter_ble_maintenance_from_awake_window();
                                return;   /* not reached */
                            }
                        }
                        /* A NEW press: fresh id, and repost at once so home
                         * sees it on its very next relay poll. */
                        uint64_t ev = ++s_button_event_seq;
                        ESP_LOGI(TAG, "window press '%s' (event %llu)",
                                 button_name(b), (unsigned long long)ev);
                        rest_set_button(button_name(b), ev);
                        char wip[16] = {0};
                        wifi_manager_get_sta_ip(wip, sizeof wip);
                        relay_post_status(current_rssi(), wip, pw, ph,
                                          FW_VERSION);
                        win_s = rest_config_get()->button_wake_s;
                        if (win_s <= 0) break;
                        deadline = esp_timer_get_time() +
                                   (int64_t)win_s * 1000000;
                        next_poll = 0;          /* check for the render now */
                    }
                    if (esp_timer_get_time() >= next_poll) {
                        if (relay_poll_and_paint()) cfg_dirty = true;
                        next_poll = esp_timer_get_time() +
                                    RELAY_BUTTON_POLL_MS * 1000LL;
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                ESP_LOGI(TAG, "relay button window closed");
            }
#endif
        }

        /* A revoked pairing is terminal, not transient (see relay.h). Drop it,
         * leave the last image on the glass, and reopen setup so a fresh code
         * re-pairs cleanly -- polling a dead mailbox forever would otherwise
         * look identical to "home has published nothing". */
        if (relay_pairing_revoked()) {
            relay_forget_revoked_pairing();
            rest_set_button(NULL, 0);
            wifi_sta_stop();
            run_provisioning_then_reboot("Relay pairing was revoked");
            return;   /* not reached */
        }

        rest_set_button(NULL, 0);   /* don't leak the press into a later wake */
        if (cfg_dirty) rest_config_save();
        wifi_sta_stop();
        sleep_forever_or_until_timer();
        return;   /* not reached */
    }

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
            /* Radio is up and the new frame's digest is known: fetch the
             * overlay spec + atlases for it (404 = dormant, no-op). */
            if (frame != NULL) {
                overlay_frame_downloaded(new_etag);
                proto2_frame_downloaded(new_etag, fo.manifest_digest,
                                        fo.manifest_url);
                touch3_frame_downloaded(new_etag, fo.layout_digest);
            }
        } else {
            ESP_LOGE(TAG, "frame fetch failed for %s", fullurl);
        }
    } else if (fs == REST_NOT_MODIFIED) {
        ESP_LOGI(TAG, "frame unchanged (304); skipping paint");
        skip_paint = true;
        /* The touch spec normally rides a frame download, so a page whose image
         * never changes would never be re-checked: turning controls on
         * server-side would go unnoticed forever. Re-ask while we hold none.
         * Gaining controls means the frame on glass was composed without them,
         * so drop the cached ETag to force a 200 + repaint on the next poll. */
#if BOARD_TOUCH3
        if (!touch3_active()) {
            touch3_poll_spec();
            if (touch3_take_repaint()) {
                ESP_LOGI(TAG, "touch spec gained controls; forcing a repaint");
                rest_config_set_frame_etag("");
                cfg_dirty = true;
                if (fetch_and_paint_current(rest_config_get()->server_url)) {
                    skip_paint = true;   /* that call already painted */
                }
            }
        }
#endif
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
            /* wake_at is the same instant as next_poll_s, absolute. One-shot,
             * RAM only; the sleep path converts it to a delta at sleep entry
             * so the paint below doesn't push the wake late (wake_align.h). */
            wake_align_set_target(so.wake_at);
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
#ifdef BOARD_BUZZER_PIN
            /* Buzzer config, same channel and the same absent-means-keep rule
             * as the touch fields above (#258). */
            if (so.beep_enabled >= 0 || so.beep_volume >= 0 || so.beep_pattern[0]) {
                const rest_config_t *bc = rest_config_get();
                bool    en  = (so.beep_enabled >= 0) ? (so.beep_enabled != 0) : bc->beep_enabled;
                int32_t vol = (so.beep_volume  >= 0) ? so.beep_volume        : bc->beep_volume;
                const char *pat = so.beep_pattern[0] ? so.beep_pattern : bc->beep_pattern;
                if (en != bc->beep_enabled || vol != bc->beep_volume ||
                    strcmp(pat, bc->beep_pattern) != 0) {
                    rest_config_set_beep(en, pat, vol);
                    cfg_dirty = true;
                    ESP_LOGI(TAG, "beep config: enabled=%d tone=%s volume=%ld",
                             en, rest_config_get()->beep_pattern,
                             (long)rest_config_get()->beep_volume);
                }
            }
#endif
#if BOARD_OVERLAY_PARTIAL
            /* overlay_values may ride the status response; patches too, but
             * those are deferred until after the paint (see pending_patches). */
            if (so.overlay_values[0]) {
                overlay_ingest_values(so.overlay_values, strlen(so.overlay_values));
                proto2_ingest_values(so.overlay_values, strlen(so.overlay_values));
                /* v3 reconcile also rides the polled envelope, so a battery
                 * device (no SSE) still corrects its drawn state on each wake. */
                touch3_ingest_values(so.overlay_values, strlen(so.overlay_values));
            }
            proto2_note_clock(so.server_time, so.local_hh, so.local_mm);
            if (so.sync_obj[0])
                proto2_note_sync(so.sync_obj, strlen(so.sync_obj));
            if (so.overlay_patches[0])
                snprintf(pending_patches, sizeof pending_patches, "%s",
                         so.overlay_patches);
            /* A touch wake hit-tests and feeds back before the radio is up, so
             * its /interact report queued. The radio is up now: drain it here
             * rather than relying on a linger window existing (battery mode has
             * none -- firmware-spec §11 wake -> report -> sleep). */
            touch3_flush_reports();
#endif
            /* Deck resync signal: decided here, executed at the tail of the
             * wake (after painting + reporting, radio still up). */
            if (so.deck_present) {
                snprintf(deck_srv_ver, sizeof deck_srv_ver, "%s", so.deck_version);
                deck_sync_needed = deck_sync_pending(true, deck_srv_ver);
            }
            if (so.collection_present) {
                snprintf(collection_id, sizeof collection_id, "%s",
                         so.collection_id);
                snprintf(collection_kind, sizeof collection_kind, "%s",
                         so.collection_kind);
                snprintf(collection_srv_ver, sizeof collection_srv_ver, "%s",
                         so.collection_version);
            }
            collection_sync_needed = collection_sync_pending(
                so.collection_present, collection_id, collection_kind,
                collection_srv_ver);
            collection_network_polled(effective_sleep_s());
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
    if (!will_linger && !deck_sync_needed && !collection_sync_needed &&
        !proto2_sync_pending())
        wifi_sta_stop();

    if (frame != NULL) {
        ESP_LOGI(TAG, "painting downloaded frame (~30 s)...");
        ESP_ERROR_CHECK(epd_port_init());
        epd_init();
        touch3_compose(frame);   /* draw v3 controls into their blank rects */
        epd_display(frame);
        epd_sleep();
        if (new_etag[0]) { rest_config_set_frame_etag(new_etag); cfg_dirty = true; }
        rest_config_set_ui_state(UI_CONNECTED);   /* a real frame is up now */
        overlay_after_paint(frame, new_etag);      /* keep base copy + SD patches */
        proto2_frame_painted(new_etag);   /* server-wins: full frame clears the ledger */
        touch3_after_paint(new_etag);
        free(frame);   /* AFTER after_paint: it memcpys the frame into its buffers */
        deck_network_painted();   /* SD-paint report no longer describes the display */
        /* Tesserae currently uses the 16-lower-hex frame content digest as its
         * ETag. Only infer an Album slot while that equivalence is explicit;
         * weak/quoted/future ETags still interrupt, but cannot corrupt shuffle
         * bookkeeping through a silent format coupling. */
        const char *album_digest = fc_digest_valid(new_etag) ? new_etag : NULL;
        if (new_etag[0] && !album_digest)
            ESP_LOGW(TAG, "frame ETag is not an Album digest; resuming without slot inference");
        collection_network_painted(album_digest);
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

#if BOARD_OVERLAY_PARTIAL
    /* Deferred status-borne patch doc: buffers are live now on a paint wake
     * (partial-refreshes the patched rects on top of the just-painted base).
     * On a 304 wake with only the sparse SD restore, the ingest falls back
     * to clearing the ETag; the next wake's full download converges. */
    if (pending_patches[0])
        overlay_ingest_patches(pending_patches, strlen(pending_patches));
#endif

    /* Deck cache sync tail (contract: AFTER painting and reporting): the
     * radio was kept up for this; fetch the manifest, diff, fetch missing
     * frames, delete orphans. Then finish the deferred radio-down. */
    if (deck_sync_needed) {
        deck_sync_tail(true, deck_srv_ver);
        if (!will_linger && !collection_sync_needed && !proto2_sync_pending())
            wifi_sta_stop();
    }
    if (collection_sync_needed) {
        collection_sync_tail(true, collection_id, collection_kind,
                             collection_srv_ver);
        if (!will_linger && !proto2_sync_pending()) wifi_sta_stop();
    }
    /* proto2 bundle resync (contract: after painting + reporting). */
    if (proto2_sync_pending()) {
        proto2_sync_tail();
        if (!will_linger) wifi_sta_stop();
    }

#if BOARD_HAS_TOUCH
    /* Touch linger: stay awake touch_linger_s after the interaction, polling the
     * GT911 INT and firing further touch GETs at full responsiveness (no deep
     * sleep + boot + reconnect between rapid touches). WiFi is still up here.
     * The window resets on each interaction; it ends when idle for the window. */
    if (will_linger) {
        int linger_s = rest_config_get()->touch_linger_s;
#if BOARD_OVERLAY_PARTIAL
        /* Schema 2: the post-action patch lands a couple of seconds after
         * the tap's HA call round-trips; a short linger would sleep through
         * it and the digest never changes to wake us later. */
        if (linger_s < 10) linger_s = 10;
#endif
        ESP_LOGI(TAG, "touch linger: up to %d s awake for further touches", linger_s);
        int64_t deadline = esp_timer_get_time() + (int64_t)linger_s * 1000000;
        while (esp_timer_get_time() < deadline) {
            if (!touch_int_asserted()) {
                /* Values slots + schema-2 patches (overlay): poll ~1 s ONLY
                 * while awake in this window; changed rects partial-refresh
                 * in place. A patch the poll couldn't honour falls back to
                 * one normal /frame poll (the contract's only fallback). */
                overlay_linger_poll();
                proto2_flush_reports();
                touch3_flush_reports();
                proto2_linger_tick();   /* local:clock minute re-blit */
                bool forced = overlay_take_refetch();
                bool stale  = overlay_take_stale();
                if (forced || stale) {
                    /* The digest never changes under schema 2, so a forced
                     * repaint needs the cached ETag dropped to get a 200. A
                     * stale-frame refetch keeps its ETag: the digest moved,
                     * so the conditional GET answers 200 on its own, and
                     * holding it means a frame that lands back on the old
                     * digest mid-fetch still 304s. */
                    if (forced) rest_config_set_frame_etag("");
                    cfg_dirty = true;
                    if (fetch_and_paint_current(rest_config_get()->server_url)) {
                        /* The fallback full repaint can outlive the linger
                         * window; keep it open a few more polls so the patch
                         * that forced the refetch can now apply onto the live
                         * buffers (and taps latched during the paint still
                         * get serviced). */
                        int64_t floor_us = esp_timer_get_time() + 6 * 1000000LL;
                        if (deadline < floor_us) deadline = floor_us;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            touch_stroke_t st;
            touch_capture_stroke_cb(&st, TOUCH_FIRST_POINT_MS, TOUCH_CAP_MS,
                                    touch3_stroke_sample, NULL);
            if (!st.valid) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            buzzer_feedback();   /* same reason as the wake path (#258) */
            bool p2_poll = false;
            if (touch3_try_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                                 &p2_poll)) {
                /* v3 owned it: feedback is already on glass and the report is
                 * sent. Only a tier-2/nav action needs a fresh frame. */
                if (p2_poll &&
                    fetch_and_paint_current(rest_config_get()->server_url))
                    cfg_dirty = true;
            } else if (proto2_try_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                                 &p2_poll)) {
                /* v2 owned it. Tier 0/1 results ride the 1 s patch polls;
                 * nav/refresh/tier-2 want an actual frame poll. */
                if (p2_poll &&
                    fetch_and_paint_current(rest_config_get()->server_url))
                    cfg_dirty = true;
            } else {
                /* v1 chain: echo, coordinate dispatch, frame poll. */
                overlay_try_echo(st.x1, st.y1);
                uint64_t ev = ++s_button_event_seq;
                ESP_LOGI(TAG, "linger touch (%d,%d)->(%d,%d) %ums (event %llu)",
                         st.x0, st.y0, st.x1, st.y1, (unsigned)st.ms,
                         (unsigned long long)ev);
                rest_set_touch(st.x0, st.y0, st.x1, st.y1, st.ms,
                               rest_config_get()->last_frame_etag, ev);
                if (fetch_and_paint_current(rest_config_get()->server_url))
                    cfg_dirty = true;
            }
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
            if (buttons_is_maintenance_button(b)) {
                if (buttons_maintenance_held_for_activation()) {
                    enter_ble_maintenance_from_awake_window();
                    return;   /* not reached */
                }
            }
            uint64_t ev = ++s_button_event_seq;
            ESP_LOGI(TAG, "window press '%s' (event %llu)", button_name(b),
                     (unsigned long long)ev);
            rest_set_button(button_name(b), ev);
            buzzer_feedback();
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
