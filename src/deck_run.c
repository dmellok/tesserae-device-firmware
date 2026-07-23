/* deck_run.c -- wake-loop orchestration for the SD deck cache. See header. */

#include "deck_run.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "buttons.h"
#include "deck.h"
#include "deck_cache.h"
#include "epd_driver.h"
#include "image_fetcher.h"
#include "rest_config.h"
#include "sdcard.h"

static const char *TAG = "deck";

/* Nav state: the RTC copy survives deep sleep without flash writes; the NVS
 * mirror (rest_config) recovers it after a RESET. */
RTC_DATA_ATTR static char    s_rtc_deck[DECK_ID_CAP];
RTC_DATA_ATTR static char    s_rtc_page[DECK_ID_CAP];
RTC_DATA_ATTR static uint8_t s_rtc_valid;

/* Loaded once per wake by deck_boot() when eligible; s_have_deck gates all
 * local nav. Heap-allocated: ~21 KB is too big for static internal RAM. */
static deck_manifest_t *s_manifest;
static bool             s_have_deck;

/* One local SD paint per wake maximum needs port_init/init once. */
static bool s_panel_up;

static void nav_persist(const char *deck_id, const char *page_id, bool sd_painted)
{
    snprintf(s_rtc_deck, sizeof s_rtc_deck, "%s", deck_id);
    snprintf(s_rtc_page, sizeof s_rtc_page, "%s", page_id);
    s_rtc_valid = 1;
    rest_config_set_deck_nav(deck_id, page_id);
    rest_config_set_deck_sd_painted(sd_painted);
    rest_config_save();
}

void deck_boot(void)
{
    if (!sdcard_mount()) return;
    rest_set_deck_capability(sdcard_free_bytes());

    const rest_config_t *c = rest_config_get();

    /* RTC nav state is authoritative across deep sleeps; fall back to the NVS
     * mirror after a RESET (RTC RAM lost). */
    if (s_rtc_valid) {
        rest_config_set_deck_nav(s_rtc_deck, s_rtc_page);
    } else if (c->deck_id[0]) {
        snprintf(s_rtc_deck, sizeof s_rtc_deck, "%s", c->deck_id);
        snprintf(s_rtc_page, sizeof s_rtc_page, "%s", c->deck_page);
        s_rtc_valid = 1;
    }

    /* The displayed frame came from SD on an earlier wake: keep the report
     * fields armed until a network frame replaces it (contract: "the next
     * /status body"). */
    if (c->deck_sd_painted && c->deck_page[0])
        rest_set_deck_painted(c->deck_page, c->deck_synced_ver);

    /* Local nav eligibility: a fully-synced deck at the server's last-known
     * current version. Any miss just means network behaviour. */
    if (!c->deck_id[0] || !c->deck_synced_ver[0]) return;
    if (strcmp(c->deck_synced_ver, c->deck_srv_ver) != 0) return;

    s_manifest = malloc(sizeof *s_manifest);
    if (!s_manifest) return;
    if (!deck_cache_load_manifest(c->deck_id, s_manifest) ||
        strcmp(s_manifest->version, c->deck_synced_ver) != 0 ||
        strcmp(s_manifest->deck_id, c->deck_id) != 0) {
        free(s_manifest);
        s_manifest = NULL;
        return;
    }
    s_have_deck = true;
    ESP_LOGI(TAG, "deck '%s' v%s ready (%d pages), page '%s'",
             c->deck_id, s_manifest->version, s_manifest->n_pages, c->deck_page);
}

/* Serve `target` from the cache if it is cached, verified, sized for THIS
 * panel, and within ttl. Paints and persists nav state on success. */
static bool serve_page_from_sd(const char *target)
{
    const deck_page_t *p = deck_find_page(s_manifest, target);
    if (!p) return false;

    /* The manifest's bytes field is the wire size for this kind; a frame that
     * is not exactly this panel's buffer cannot be painted here. */
    if (p->bytes != EPD_BUF_BYTES) {
        ESP_LOGW(TAG, "page '%s' is %u bytes, panel wants %u; network fallback",
                 target, (unsigned)p->bytes, (unsigned)EPD_BUF_BYTES);
        return false;
    }
    if (p->ttl_s > 0) {
        int32_t age = deck_cache_frame_age_s(s_manifest->deck_id, p->digest);
        if (age > p->ttl_s) {
            ESP_LOGI(TAG, "page '%s' cached copy stale (age %ld > ttl %ld)",
                     target, (long)age, (long)p->ttl_s);
            return false;
        }
    }

    uint8_t *frame = NULL;
    if (!deck_cache_read_frame(s_manifest->deck_id, p->digest, p->bytes, &frame))
        return false;   /* missing or failed verification (file deleted) */

    ESP_LOGI(TAG, "painting page '%s' from SD (radio off)", target);
    if (!s_panel_up) {
        if (epd_port_init() != ESP_OK) { free(frame); return false; }
        s_panel_up = true;
    }
    epd_init();
    epd_display(frame);
    epd_sleep();
    free(frame);

    nav_persist(s_manifest->deck_id, target, true);
    rest_set_deck_painted(target, s_manifest->version);
    return true;
}

static bool try_nav_event(const char *button, int tx, int ty, bool is_touch)
{
    if (!s_have_deck) return false;
    const char *cur = rest_config_get()->deck_page;
    if (!cur[0]) return false;

    const char *target = is_touch
        ? deck_nav_touch(s_manifest, cur, tx, ty, EPD_WIDTH, EPD_HEIGHT)
        : deck_nav_button(s_manifest, cur, button);
    if (!target) return false;
    return serve_page_from_sd(target);
}

bool deck_try_button(const char *button, uint32_t *event_seq, bool *fallthrough)
{
    *fallthrough = false;
    if (!button || !try_nav_event(button, 0, 0, false)) return false;

    /* Served locally: withdraw the pending report main armed for this press
     * (contract: locally-navigated events are NOT sent as button actions). */
    rest_set_button("", 0);

#ifdef BOARD_HAS_BUTTONS
    /* Local linger (issue #123 semantics, radio off): keep serving presses
     * that hit cached links for button_wake_s after each paint. A press with
     * no local link arms the normal report and falls through to the network
     * cycle; the window also honours the same total cap as the online path. */
    int32_t window_s = rest_config_get()->button_wake_s;
    if (window_s > 0) {
        buttons_poll_init();
        int64_t total_ms = 0, idle_ms = 0;
        while (idle_ms < window_s * 1000 &&
               total_ms < (int64_t)BUTTON_WINDOW_CAP_S * 1000) {
            vTaskDelay(pdMS_TO_TICKS(20));
            idle_ms += 20; total_ms += 20;
            button_id_t b = buttons_poll_pressed();
            if (b == BTN_NONE) continue;
            const char *name = button_name(b);
            uint32_t ev = ++(*event_seq);
            if (try_nav_event(name, 0, 0, false)) {
                ESP_LOGI(TAG, "local nav '%s' (event %u)", name, (unsigned)ev);
                idle_ms = 0;   /* window restarts after each served press */
            } else {
                /* No cached link for this press: today's behaviour, over the
                 * network, carrying this event. */
                rest_set_button(name, ev);
                *fallthrough = true;
                return true;
            }
        }
    }
#else
    (void)event_seq;
#endif
    return true;
}

bool deck_try_touch(int x, int y)
{
    return try_nav_event(NULL, x, y, true);
}

void deck_network_painted(void)
{
    if (!rest_config_get()->deck_sd_painted) return;
    rest_config_set_deck_sd_painted(false);
    rest_set_deck_painted(NULL, NULL);
    rest_config_save();
}

bool deck_sync_pending(bool deck_present, const char *version)
{
    if (!sdcard_mounted() || !deck_present) return false;
    /* Version match with a loaded deck means fully synced; anything else
     * (no deck yet, version drift, unreadable manifest) wants the tail. */
    const rest_config_t *c = rest_config_get();
    if (version[0] &&
        strcmp(version, c->deck_synced_ver) == 0 && s_have_deck)
        return false;
    return true;
}

void deck_sync_tail(bool deck_present, const char *version)
{
    if (!deck_sync_pending(deck_present, version)) return;

    /* Remember the server's announced version regardless of how far the sync
     * gets: local nav is gated on synced_ver == srv_ver. */
    rest_config_set_deck_srv_ver(version);

    char *json = malloc(16 * 1024);
    if (!json) { rest_config_save(); return; }
    size_t len = 0;
    rest_status_t st = rest_get_deck_manifest(json, 16 * 1024, &len, 15000);

    if (st == REST_NO_CONTENT) {
        /* No deck bound: drop nav state, KEEP the cached files for reuse. */
        ESP_LOGI(TAG, "no deck bound (204); dropping nav state");
        s_rtc_valid = 0; s_rtc_deck[0] = '\0'; s_rtc_page[0] = '\0';
        rest_config_set_deck_nav("", "");
        rest_config_set_deck_synced_ver("");
        rest_config_set_deck_sd_painted(false);
        rest_set_deck_painted(NULL, NULL);
        rest_config_save();
        free(json);
        return;
    }
    if (st != REST_OK) {
        ESP_LOGW(TAG, "manifest fetch failed (%d); retrying next wake", st);
        rest_config_save();
        free(json);
        return;
    }

    deck_manifest_t *m = malloc(sizeof *m);
    if (!m || !deck_manifest_parse(json, len, m)) {
        ESP_LOGW(TAG, "manifest unparseable; deck disabled until it changes");
        rest_config_save();
        free(json); free(m);
        return;
    }
    deck_cache_save_manifest(m->deck_id, json, len);
    free(json);

    /* Diff the card against the manifest; fetch only what is missing. */
    char have[DECK_MAX_PAGES][DECK_DIGEST_HEX + 1];
    char fetch[DECK_MAX_PAGES][DECK_DIGEST_HEX + 1];
    char orphan[DECK_MAX_PAGES][DECK_DIGEST_HEX + 1];
    int n_orphan = 0;
    int n_have  = deck_cache_list(m->deck_id, have, DECK_MAX_PAGES);
    int n_fetch = deck_sync_plan(m, have, n_have,
                                 fetch, DECK_MAX_PAGES,
                                 orphan, DECK_MAX_PAGES, &n_orphan);
    ESP_LOGI(TAG, "sync %s -> v%s: %d cached, %d to fetch, %d orphans",
             m->deck_id, m->version, n_have, n_fetch, n_orphan);

    bool complete = true;
    for (int i = 0; i < n_fetch; i++) {
        const deck_page_t *page = NULL;
        for (int j = 0; j < m->n_pages; j++)
            if (strcmp(m->pages[j].digest, fetch[i]) == 0) { page = &m->pages[j]; break; }
        if (!page) continue;

        char url[320];
        rest_deck_frame_url(fetch[i], url, sizeof url);
        fetched_image_t img;
        esp_err_t err = image_fetch_auth(url, rest_bearer_token(), &img);
        if (err == ESP_ERR_NOT_FOUND) {
            /* Contract: 404 -> the manifest is already stale; re-fetch it on
             * the next wake rather than churning here. */
            ESP_LOGW(TAG, "frame %s 404; manifest stale, aborting sync", fetch[i]);
            complete = false;
            break;
        }
        if (err != ESP_OK) { complete = false; break; }
        bool ok = deck_cache_write_frame(m->deck_id, fetch[i],
                                         img.data, img.len, page->bytes);
        free(img.data);
        if (!ok) { complete = false; break; }
    }

    for (int i = 0; i < n_orphan; i++)
        deck_cache_delete(m->deck_id, orphan[i]);

    if (complete) {
        rest_config_set_deck_synced_ver(m->version);
        /* Adopt the deck / heal the current page: entry page when the deck
         * changed or the old page no longer exists. */
        const rest_config_t *c = rest_config_get();
        const char *page = c->deck_page;
        if (strcmp(c->deck_id, m->deck_id) != 0 || !deck_find_page(m, page))
            page = m->entry_page_id;
        nav_persist(m->deck_id, page, c->deck_sd_painted);
        ESP_LOGI(TAG, "deck '%s' synced at v%s", m->deck_id, m->version);
    } else {
        rest_config_save();   /* srv_ver moved; synced_ver deliberately not */
    }
    free(m);
}

void deck_pre_sleep(void)
{
    sdcard_unmount();
}
