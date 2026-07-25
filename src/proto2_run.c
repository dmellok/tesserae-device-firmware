/* proto2_run.c -- device orchestration for protocol v2. See proto2_run.h. */

#include "proto2_run.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "image_fetcher.h"

#include "deck.h"           /* deck_digest_valid (16-hex check) */
#include "epd_driver.h"
#include "panel/epd_panel.h"   /* epd_active_driver()->info.bpp */
#include "net_rest.h"
#include "overlay.h"        /* overlay_invert_rect (shared fb primitive) */
#include "overlay_run.h"    /* framebuffer + hygiene accessors */
#include "deck_cache.h"     /* digest-verified SD store (pseudo-deck p2bundle) */
#include "proto2.h"
#include "rest_config.h"
#include "sdcard.h"

static const char *TAG = "proto2";

#define P2_MANIFEST_MAX   (24 * 1024)   /* raw JSON cap (64-region manifests) */
#define P2_LEDGER_MAX     16
#define P2_RQUEUE_MAX     16
#define P2_ATLAS_MAX_B    (64 * 1024)   /* per-strip cap (contract §9) */

/* Minted by main.c (the single wake-event counter, uint64, <= 2^53). */
extern uint64_t app_next_event_id(void);

static void bundle_track_page(const char *frame_digest);   /* fwd */

/* ---- state (statics: the linger call chain must stay off the stack) ---- */

static p2_manifest_t s_man;
static bool          s_have_man;
static char         *s_raw_man;         /* raw JSON kept for the SD cache */
static size_t        s_raw_man_len;
static uint8_t      *s_atlas_bits[P2_MAX_ATLASES];  /* PSRAM-resident strips */
static int64_t       s_values_seq = -1; /* v2 values stream high-water */

/* local: clock discipline (from /status). The offset converts the RTC's
 * unix time into server-local wall minutes without any tz database. */
static bool    s_clock_ok;
static int32_t s_clock_off_min;         /* local = utc_minutes + off */
static int     s_clock_last_min = -1;   /* last rendered minute-of-day */

/* Server generation probe, cached per boot (a wake = a connection epoch):
 * 0 unknown, +1 v2 (manifest block seen), -1 v1 (200 without the block). */
static int s_epoch;

/* State bundle (§8): tier-0 nav frames + toggle tiles, stored through
 * deck_cache under a reserved pseudo-deck id (digest+length verified,
 * tmp+rename writes, mtime for ttl). */
#define P2_BUNDLE_DECK "p2bundle"
#define P2_BUNDLE_JSON SDCARD_MOUNT_POINT "/tesserae/decks/" P2_BUNDLE_DECK                        "/manifest.json"
static p2_bundle_t s_bundle;
static bool s_have_bundle;
static bool s_bundle_sync;              /* sync envelope requested a resync */
static char s_cur_page[P2_ID_CAP];      /* bundle state id on glass, "" unknown */
static int8_t s_cycle_idx[P2_MAX_REGIONS];   /* tile cycle position, -1 unknown */

/* Optimistic ledger (§6). Echo inversions and tier-1 optimistic pixels
 * share it: both are "device composites awaiting a covering server
 * artifact", both clear on overlap, neither self-reverts. */
typedef struct {
    bool active;
    char region_id[P2_ID_CAP];
    int  x, y, w, h;
    int64_t t_applied_us;
} p2_ledger_t;
static p2_ledger_t s_ledger[P2_LEDGER_MAX];

/* /tap report queue: replayed in order; event-id dedup makes retries safe.
 * RAM-only -- reports queue when a send fails mid-linger and connectivity
 * virtually always returns within the window; a report pending at sleep is
 * logged and dropped (flagged in the v2 report). */
typedef struct {
    bool     used;
    char     region_id[P2_ID_CAP];
    char     gesture[16];
    int      value;                     /* -1 = absent */
    char     digest[P2_DIGEST_HEX + 1];
    uint64_t event_id;
    int      x0, y0, x1, y1;
} p2_report_t;
static p2_report_t s_rq[P2_RQUEUE_MAX];
static int s_rq_head, s_rq_len;

/* ---- SD manifest cache: /tesserae/proto2/<frame_digest>.man ---- */

static bool man_path(char *out, size_t cap, const char *digest)
{
    if (!sdcard_mounted() || !deck_digest_valid(digest)) return false;
    int n = snprintf(out, cap, SDCARD_MOUNT_POINT "/tesserae/proto2/%s.man",
                     digest);
    return n > 0 && n < (int)cap;
}

static void man_cache_write(const char *digest)
{
    char path[128];
    if (!s_raw_man || !man_path(path, sizeof path, digest)) return;
    mkdir(SDCARD_MOUNT_POINT "/tesserae", 0775);
    mkdir(SDCARD_MOUNT_POINT "/tesserae/proto2", 0775);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s_raw_man, 1, s_raw_man_len, f);
    fclose(f);
}

/* ---- ledger ---- */

static void ledger_clear_all(void)
{
    memset(s_ledger, 0, sizeof s_ledger);
}

static void ledger_add(const p2_region_t *r)
{
    /* Overflow: oldest entry downgrades to "awaiting server" -- its pixels
     * stay on glass, we just stop tracking it (contract cap). */
    int oldest = 0;
    int64_t t_min = INT64_MAX;
    for (int i = 0; i < P2_LEDGER_MAX; i++) {
        if (!s_ledger[i].active) { oldest = i; t_min = INT64_MIN; break; }
        if (s_ledger[i].t_applied_us < t_min) {
            t_min = s_ledger[i].t_applied_us;
            oldest = i;
        }
    }
    p2_ledger_t *e = &s_ledger[oldest];
    e->active = true;
    snprintf(e->region_id, sizeof e->region_id, "%s", r->id);
    e->x = r->x; e->y = r->y; e->w = r->w; e->h = r->h;
    e->t_applied_us = esp_timer_get_time();
}

static bool rects_overlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* overlay_run calls this after applying schema-2 patch rects. */
void proto2_note_patch_rects(const overlay_patch_rect_t *r, int n)
{
    /* A server artifact over a tile region makes its cycle parity unknown
     * again (the server may have rendered either state). */
    for (int i = 0; i < s_man.n_regions; i++) {
        if (s_cycle_idx[i] < 0) continue;
        for (int j = 0; j < n; j++)
            if (rects_overlap(s_man.regions[i].x, s_man.regions[i].y,
                              s_man.regions[i].w, s_man.regions[i].h,
                              r[j].x, r[j].y, r[j].w, r[j].h)) {
                s_cycle_idx[i] = -1;
                break;
            }
    }
    for (int i = 0; i < P2_LEDGER_MAX; i++) {
        if (!s_ledger[i].active) continue;
        for (int j = 0; j < n; j++)
            if (rects_overlap(s_ledger[i].x, s_ledger[i].y,
                              s_ledger[i].w, s_ledger[i].h,
                              r[j].x, r[j].y, r[j].w, r[j].h)) {
                ESP_LOGD(TAG, "ledger '%s' cleared by patch rect",
                         s_ledger[i].region_id);
                s_ledger[i].active = false;
                break;
            }
    }
}

void proto2_frame_painted(const char *digest)
{
    ledger_clear_all();   /* a full frame covers every optimistic rect */
    memset(s_cycle_idx, -1, sizeof s_cycle_idx);   /* tile parity unknown */
    if (digest && digest[0] && s_have_bundle) bundle_track_page(digest);
}

/* ---- reports ---- */

static void rq_push(const p2_report_t *rep)
{
    if (s_rq_len >= P2_RQUEUE_MAX) {         /* drop-oldest */
        s_rq_head = (s_rq_head + 1) % P2_RQUEUE_MAX;
        s_rq_len--;
    }
    s_rq[(s_rq_head + s_rq_len) % P2_RQUEUE_MAX] = *rep;
    s_rq[(s_rq_head + s_rq_len) % P2_RQUEUE_MAX].used = true;
    s_rq_len++;
}

static bool report_send(const p2_report_t *rep)
{
    char outcome[24];
    rest_status_t st = rest_post_tap(rep->region_id, rep->gesture, rep->value,
                                     rep->digest, rep->event_id,
                                     rep->x0, rep->y0, rep->x1, rep->y1,
                                     outcome, sizeof outcome, 3000);
    if (st == REST_OK) {
        if (strcmp(outcome, "ha_failed") == 0)
            ESP_LOGW(TAG, "'%s': ha_failed (correction will repaint truth)",
                     rep->region_id);
        else if (outcome[0] && strcmp(outcome, "ok") != 0)
            ESP_LOGI(TAG, "'%s': outcome %s", rep->region_id, outcome);
        return true;
    }
    /* 4xx/5xx/timeout/net: queue for in-order replay (dedup-safe). 401 is
     * the exception -- re-pair territory, retrying is noise. */
    if (st == REST_UNAUTH) return true;
    return false;
}

void proto2_flush_reports(void)
{
    while (s_rq_len > 0) {
        p2_report_t *rep = &s_rq[s_rq_head];
        if (!report_send(rep)) return;       /* still down; keep order */
        rep->used = false;
        s_rq_head = (s_rq_head + 1) % P2_RQUEUE_MAX;
        s_rq_len--;
    }
}

static void report_or_queue(const p2_report_t *rep)
{
    proto2_flush_reports();                  /* keep order: drain first */
    if (s_rq_len > 0 || !report_send(rep)) rq_push(rep);
}

/* ---- atlas strips (fetch + SD cache + digest verify; PSRAM resident) ---- */

static bool atlas_path(char *out, size_t cap, const char *digest)
{
    if (!sdcard_mounted() || !deck_digest_valid(digest)) return false;
    int n = snprintf(out, cap,
                     SDCARD_MOUNT_POINT "/tesserae/proto2/atlas-%s.bin",
                     digest);
    return n > 0 && n < (int)cap;
}

static bool atlas_digest_ok(const uint8_t *bits, size_t len, const char *digest)
{
    uint8_t sha[32];
    char hex[DECK_DIGEST_HEX + 1];
    deck_sha256(bits, len, sha);
    deck_digest_hex16(sha, hex);
    return strcmp(hex, digest) == 0;
}

/* Expected packed strip size: total width always even (contract §9);
 * tolerate an odd parse by rounding the stride up. */
static size_t atlas_expected_len(const p2_atlas_t *a)
{
    return (size_t)((a->strip_w + 1) / 2) * (size_t)a->height;
}

static void free_atlases(void)
{
    for (int i = 0; i < P2_MAX_ATLASES; i++) {
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = NULL;
    }
}

/* Attach bits for every manifest atlas: SD first, network second. A text
 * region whose atlas fails stays parsed but unrenderable (draw no-ops). */
static void load_atlases(void)
{
    for (int i = 0; i < s_man.n_atlases; i++) {
        p2_atlas_t *a = &s_man.atlases[i];
        size_t want = atlas_expected_len(a);
        if (want == 0 || want > P2_ATLAS_MAX_B) continue;

        char path[144];
        uint8_t *bits = NULL;
        if (atlas_path(path, sizeof path, a->digest)) {
            FILE *f = fopen(path, "rb");
            if (f) {
                bits = malloc(want + 1);
                size_t got = bits ? fread(bits, 1, want + 1, f) : 0;
                fclose(f);
                if (!bits || got != want ||
                    !atlas_digest_ok(bits, got, a->digest)) {
                    free(bits); bits = NULL;
                    unlink(path);
                }
            }
        }
        if (!bits) {
            char url[320];
            if (a->url[0] == '/')
                snprintf(url, sizeof url, "%s%s",
                         rest_config_get()->server_url, a->url);
            else
                snprintf(url, sizeof url, "%s", a->url);
            fetched_image_t img;
            if (image_fetch_auth(url, rest_bearer_token(), &img) != ESP_OK)
                continue;
            if (img.len != want || !atlas_digest_ok(img.data, img.len,
                                                    a->digest)) {
                ESP_LOGW(TAG, "atlas %s wrong size/digest", a->digest);
                free(img.data);
                continue;
            }
            bits = img.data;
            if (atlas_path(path, sizeof path, a->digest)) {
                mkdir(SDCARD_MOUNT_POINT "/tesserae", 0775);
                mkdir(SDCARD_MOUNT_POINT "/tesserae/proto2", 0775);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(bits, 1, want, f); fclose(f); }
            }
        }
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = bits;
        a->bits = bits;
    }
}

/* ---- text regions: values + local clock + slider live text ---- */

static void draw_text_region(p2_text_t *t, const char *str)
{
    bool full = false;
    uint8_t *work = overlay_work_fb(&full);
    if (!work || !full) return;
    const p2_atlas_t *a = &s_man.atlases[t->atlas_idx];
    if (!a->bits) return;
    snprintf(t->value, sizeof t->value, "%s", str);
    p2_draw_text(work, EPD_WIDTH, EPD_HEIGHT, t, a, str);
    overlay_partial_refresh(t->x, t->y, t->w, t->h, true /* DU */);
}

void proto2_ingest_values(const char *json, size_t len)
{
    if (!s_have_man || s_man.n_text == 0 || !json || !len) return;
    if (!p2_manifest_matches(&s_man, rest_config_get()->last_frame_etag))
        return;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    do {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        if (!cJSON_IsNumber(seq)) break;
        int64_t sq = (int64_t)seq->valuedouble;
        if (s_values_seq >= 0 && sq <= s_values_seq) break;  /* newest wins */
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
        if (!cJSON_IsObject(values)) break;
        s_values_seq = sq;

        for (int i = 0; i < s_man.n_text; i++) {
            p2_text_t *t = &s_man.text[i];
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, t->key);
            if (!cJSON_IsString(v) || !v->valuestring) continue;
            if (strcmp(v->valuestring, t->value) == 0) continue;
            draw_text_region(t, v->valuestring);
            ESP_LOGI(TAG, "text '%s' = \"%s\"", t->id, t->value);
        }
    } while (0);
    cJSON_Delete(root);
}

void proto2_note_clock(uint32_t server_time, int local_hh, int local_mm)
{
    if (!server_time || local_hh < 0 || local_mm < 0) return;
    int utc_min = (int)((server_time / 60) % (24 * 60));
    int loc_min = local_hh * 60 + local_mm;
    s_clock_off_min = loc_min - utc_min;   /* may wrap; normalised on use */
    s_clock_ok = true;
}

/* Render one local: key. Format "local:clock:HH.MM" -> "14.05" style. */
static void render_local_clock(p2_text_t *t)
{
    if (!s_clock_ok) return;
    time_t now = time(NULL);
    if (now < 1000000000) return;          /* RTC undisciplined */
    int m = (int)((now / 60 + s_clock_off_min) % (24 * 60));
    if (m < 0) m += 24 * 60;
    const char *fmt = t->key + strlen("local:clock:");
    char sep = strchr(fmt, ':') ? ':' : '.';
    char out[8];
    snprintf(out, sizeof out, "%02d%c%02d", m / 60, sep, m % 60);
    if (strcmp(out, t->value) != 0) draw_text_region(t, out);
}

void proto2_linger_tick(void)
{
    if (!s_have_man || !s_clock_ok) return;
    time_t now = time(NULL);
    int cur = (int)((now / 60 + s_clock_off_min) % (24 * 60));
    if (cur == s_clock_last_min) return;
    s_clock_last_min = cur;
    for (int i = 0; i < s_man.n_text; i++)
        if (strncmp(s_man.text[i].key, "local:clock:", 12) == 0)
            render_local_clock(&s_man.text[i]);
}

/* ---- state bundle: sync, tiles, tier-0 nav sources ---- */

/* Track which bundle page is on glass (frame states map digest -> page). */
static void bundle_track_page(const char *frame_digest)
{
    s_cur_page[0] = '\0';
    for (int i = 0; i < s_bundle.n_states; i++)
        if (s_bundle.states[i].kind == P2_BK_FRAME &&
            strncmp(s_bundle.states[i].digest, frame_digest,
                    P2_DIGEST_HEX) == 0) {
            snprintf(s_cur_page, sizeof s_cur_page, "%s",
                     s_bundle.states[i].state_id);
            return;
        }
}

static void bundle_restore_sd(void)
{
    if (!sdcard_mounted()) return;
    FILE *f = fopen(P2_BUNDLE_JSON, "rb");
    if (!f) return;
    char *buf = malloc(P2_MANIFEST_MAX);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, P2_MANIFEST_MAX - 1, f);
    bool complete = feof(f);
    fclose(f);
    if (complete && n &&
        p2_bundle_parse(buf, n, EPD_BUF_BYTES, &s_bundle)) {
        s_have_bundle = true;
        bundle_track_page(rest_config_get()->last_frame_etag);
    }
    free(buf);
}

/* Radio-up bundle sync: fetch the manifest, diff member digests against
 * the SD inventory, fetch only unknown digests (verified writes), delete
 * orphans. 404/204 = no bundle: drop everything (nav degrades to tier 2). */
static void bundle_sync(void)
{
    s_bundle_sync = false;
    char *buf = malloc(P2_MANIFEST_MAX);
    if (!buf) return;
    size_t len = 0;
    rest_status_t st = rest_get_bundle(buf, P2_MANIFEST_MAX, &len, 8000);
    if (st != REST_OK) {
        if (st == REST_NOT_FOUND || st == REST_NO_CONTENT) {
            s_have_bundle = false;
            memset(&s_bundle, 0, sizeof s_bundle);
        }
        free(buf);
        return;
    }
    static p2_bundle_t nb;
    if (!p2_bundle_parse(buf, len, EPD_BUF_BYTES, &nb)) { free(buf); return; }

    int fetched = 0, kept = 0;
    for (int i = 0; i < nb.n_states; i++) {
        const p2_bstate_t *bs = &nb.states[i];
        if (deck_cache_frame_age_s(P2_BUNDLE_DECK, bs->digest) != INT32_MAX) {
            kept++;
            continue;   /* present + verified at read time */
        }
        char url[320];
        if (bs->url[0] == '/')
            snprintf(url, sizeof url, "%s%s",
                     rest_config_get()->server_url, bs->url);
        else
            snprintf(url, sizeof url, "%s", bs->url);
        fetched_image_t img;
        if (image_fetch_auth(url, rest_bearer_token(), &img) != ESP_OK)
            continue;
        if (img.len == bs->bytes &&
            deck_cache_write_frame(P2_BUNDLE_DECK, bs->digest,
                                   img.data, img.len, bs->bytes))
            fetched++;
        free(img.data);
    }

    /* Evict members the new manifest dropped. */
    static char present[P2_MAX_BSTATES * 2][DECK_DIGEST_HEX + 1];
    int n_present = deck_cache_list(P2_BUNDLE_DECK, present,
                                    P2_MAX_BSTATES * 2);
    for (int i = 0; i < n_present; i++) {
        bool wanted = false;
        for (int j = 0; j < nb.n_states && !wanted; j++)
            wanted = strcmp(nb.states[j].digest, present[i]) == 0;
        if (!wanted) deck_cache_delete(P2_BUNDLE_DECK, present[i]);
    }

    s_bundle = nb;
    s_have_bundle = true;
    deck_cache_save_manifest(P2_BUNDLE_DECK, buf, len);
    bundle_track_page(rest_config_get()->last_frame_etag);
    memset(s_cycle_idx, -1, sizeof s_cycle_idx);
    ESP_LOGI(TAG, "bundle %s: %d states (%d fetched, %d cached)",
             s_bundle.bundle_digest, s_bundle.n_states, fetched, kept);
    free(buf);
}

void proto2_note_sync(const char *json, size_t len)
{
    if (!json || !len) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    const cJSON *bd = cJSON_GetObjectItemCaseSensitive(root, "bundle_digest");
    if (cJSON_IsString(bd) && bd->valuestring &&
        (!s_have_bundle ||
         strncmp(bd->valuestring, s_bundle.bundle_digest,
                 P2_DIGEST_HEX) != 0)) {
        s_bundle_sync = true;
    }
    cJSON_Delete(root);
}

bool proto2_sync_pending(void)
{
    return s_bundle_sync;
}

void proto2_sync_tail(void)
{
    if (s_bundle_sync) bundle_sync();
}

/* ---- manifest lifecycle ---- */

static void drop_manifest(void)
{
    free(s_raw_man);
    s_raw_man = NULL;
    s_raw_man_len = 0;
    s_have_man = false;
    s_values_seq = -1;
    free_atlases();
    memset(&s_man, 0, sizeof s_man);
}

void proto2_boot(void)
{
    const char *digest = rest_config_get()->last_frame_etag;
    char path[128];
    if (!man_path(path, sizeof path, digest)) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(P2_MANIFEST_MAX);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, P2_MANIFEST_MAX - 1, f);
    bool complete = feof(f);
    fclose(f);
    if (!complete || n == 0 ||
        !p2_manifest_parse(buf, n, EPD_WIDTH, EPD_HEIGHT, &s_man)) {
        free(buf);
        return;
    }
    /* The file's NAME is the anchor: a re-anchored manifest is cached under
     * the new frame digest while its body still names the old one. */
    snprintf(s_man.frame_digest, sizeof s_man.frame_digest, "%s", digest);
    s_raw_man = buf;
    s_raw_man_len = n;
    s_have_man = true;
    /* Cold boot: SD-cached strips only (no radio yet); a cache miss just
     * leaves that text region dormant until the next radio-up wake. */
    load_atlases();
    memset(s_cycle_idx, -1, sizeof s_cycle_idx);
    bundle_restore_sd();
    ESP_LOGI(TAG, "manifest restored from SD for %s (%d regions, %d text)",
             digest, s_man.n_regions, s_man.n_text);
}

void proto2_frame_downloaded(const char *digest, const char *manifest_digest,
                             const char *manifest_url)
{
    if (!digest || !digest[0]) return;

    if (!manifest_digest || !manifest_digest[0]) {
        /* No manifest block on a 200: v1 server for this epoch (§10). */
        if (s_epoch != -1) ESP_LOGI(TAG, "no manifest block: v1 epoch");
        s_epoch = -1;
        drop_manifest();
        return;
    }
    s_epoch = 1;

    /* Re-anchor: layout unchanged (patches often change pixels, not
     * layout) -- keep the held manifest, just move its frame anchor. */
    if (s_have_man &&
        strncmp(s_man.manifest_digest, manifest_digest, P2_DIGEST_HEX) == 0) {
        snprintf(s_man.frame_digest, sizeof s_man.frame_digest, "%s", digest);
        man_cache_write(digest);
        ESP_LOGI(TAG, "manifest re-anchored to %s", digest);
        return;
    }

    drop_manifest();
    char *buf = malloc(P2_MANIFEST_MAX);
    if (!buf) return;
    size_t len = 0;
    /* The manifest block's url resolves to the same canonical endpoint the
     * digest query names; the query form works for both. */
    (void)manifest_url;
    rest_status_t st = rest_get_manifest(digest, buf, P2_MANIFEST_MAX, &len,
                                         8000);
    if (st != REST_OK) {
        if (st == REST_NOT_FOUND)
            ESP_LOGI(TAG, "manifest 404 for %s (no interactivity)", digest);
        else
            ESP_LOGW(TAG, "manifest fetch failed (%d)", st);
        free(buf);
        return;
    }
    if (!p2_manifest_parse(buf, len, EPD_WIDTH, EPD_HEIGHT, &s_man) ||
        !p2_manifest_matches(&s_man, digest)) {
        ESP_LOGW(TAG, "manifest unusable for %s", digest);
        memset(&s_man, 0, sizeof s_man);
        free(buf);
        return;
    }
    s_raw_man = buf;
    s_raw_man_len = len;
    s_have_man = true;
    load_atlases();
    man_cache_write(digest);
    ESP_LOGI(TAG, "manifest %s: %d regions, %d text, %d atlases",
             s_man.manifest_digest, s_man.n_regions, s_man.n_text,
             s_man.n_atlases);
}

bool proto2_active(void)
{
    return s_have_man && s_man.n_regions > 0 &&
           p2_manifest_matches(&s_man, rest_config_get()->last_frame_etag);
}

/* ---- tier engine ---- */

/* Blit the region's NEXT tile from the bundle (SD, digest-verified) into
 * the work buffer. False on any miss -- the caller falls back to invert. */
static bool apply_tile(int region_idx, const p2_region_t *r)
{
    if (!s_have_bundle || r->n_cycle < 2) return false;
    int idx = s_cycle_idx[region_idx];
    idx = (idx < 0) ? 1 : (idx + 1) % r->n_cycle;   /* unknown: frame shows [0] */

    char state_id[P2_ID_CAP + P2_NAME_CAP];
    snprintf(state_id, sizeof state_id, "%s/%s", r->fb_set, r->cycle[idx]);
    const p2_bstate_t *bs = p2_bundle_state(&s_bundle, state_id);
    if (!bs || bs->kind != P2_BK_TILE) return false;

    bool full = false;
    uint8_t *work = overlay_work_fb(&full);
    if (!work || !full) return false;

    uint8_t *blob = NULL;
    if (!deck_cache_read_frame(P2_BUNDLE_DECK, bs->digest, bs->bytes, &blob))
        return false;
    overlay_patch_rect_t pr = { .x = bs->x, .y = bs->y, .w = bs->w,
                                .h = bs->h, .offset = 0, .len = bs->bytes };
    bool ok = overlay_patch_apply_rect(work, EPD_WIDTH, EPD_HEIGHT,
                                       epd_active_driver()->info.bpp,
                                       &pr, blob, bs->bytes);
    free(blob);
    if (!ok) return false;
    overlay_partial_refresh(bs->x, bs->y, bs->w, bs->h, true /* DU */);
    s_cycle_idx[region_idx] = (int8_t)idx;
    ESP_LOGI(TAG, "tile '%s' -> %s", r->id, state_id);
    return true;
}

/* Apply the region's feedback into the work framebuffer + partial refresh.
 * Tiles blit the next cached state; everything else (and every tile miss)
 * inverts; "none" stays dark. Returns true when pixels moved (the caller
 * then ledgers them). */
static bool apply_feedback(int region_idx, const p2_region_t *r)
{
    if (r->fb == P2_FB_NONE) return false;
    if (r->fb == P2_FB_TILES && apply_tile(region_idx, r)) return true;

    bool full = false;
    uint8_t *work = overlay_work_fb(&full);
    if (!work || !full) return false;   /* cold wake: no pixel source */

    int64_t t0 = esp_timer_get_time();
    overlay_invert_rect(work, EPD_WIDTH, EPD_HEIGHT,
                        epd_active_driver()->info.bpp,
                        r->x, r->y, r->w, r->h);
    overlay_partial_refresh(r->x, r->y, r->w, r->h, true /* DU */);
    ESP_LOGI(TAG, "feedback '%s' in %lld ms (invert %d,%d %dx%d)",
             r->id, (esp_timer_get_time() - t0) / 1000,
             r->x, r->y, r->w, r->h);
    return true;
}

/* Tier-0 nav: paint the target page straight from the bundle cache. The
 * digest/manifest pointers swap to the cached state; the position report
 * still goes to the server (async). False = bundle miss/expired: caller
 * degrades to tier 2. Read+paint costs ~10-13 s on the E1003's 4 MHz
 * shared SD bus -- far off the contract's 1.5 s target, but with zero
 * network and no flash-cycling; flagged in the v2 report (a PSRAM preload
 * of the likeliest link target is the known follow-up). */
static bool nav_from_bundle(const char *target)
{
    if (!s_have_bundle || !target || !target[0]) return false;
    const p2_bstate_t *bs = p2_bundle_state(&s_bundle, target);
    if (!bs || bs->kind != P2_BK_FRAME) return false;
    if (bs->ttl_s > 0) {
        int32_t age = deck_cache_frame_age_s(P2_BUNDLE_DECK, bs->digest);
        if (age == INT32_MAX || age > bs->ttl_s) {
            ESP_LOGI(TAG, "bundle frame %s expired (age %ld)", bs->digest,
                     (long)age);
            return false;   /* stays cached until the manifest drops it */
        }
    }
    uint8_t *frame = NULL;
    if (!deck_cache_read_frame(P2_BUNDLE_DECK, bs->digest, bs->bytes, &frame))
        return false;

    int64_t t0 = esp_timer_get_time();
    if (epd_port_init() != ESP_OK) { free(frame); return false; }
    epd_init();
    epd_display(frame);
    epd_sleep();
    rest_config_set_frame_etag(bs->digest);
    rest_config_save();
    overlay_after_paint(frame, bs->digest);
    free(frame);
    ledger_clear_all();
    memset(s_cycle_idx, -1, sizeof s_cycle_idx);
    snprintf(s_cur_page, sizeof s_cur_page, "%s", target);

    /* Swap the manifest to the cached target's (SD-cached under the new
     * frame digest from an earlier visit; a miss leaves regions dormant
     * until the next radio-up wake re-fetches it). */
    drop_manifest();
    proto2_boot();
    ESP_LOGI(TAG, "tier-0 nav -> %s in %lld ms", target,
             (esp_timer_get_time() - t0) / 1000);
    return true;
}

bool proto2_try_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                      bool *want_frame_poll)
{
    if (want_frame_poll) *want_frame_poll = false;
    const char *glass = rest_config_get()->last_frame_etag;
    if (!s_have_man || !p2_manifest_matches(&s_man, glass))
        return false;   /* v1 dispatch handles it */

    p2_gesture_t g;
    int value = -1;
    int idx = p2_hit(&s_man, x0, y0, x1, y1, ms, &g, &value);
    if (idx < 0 || g == P2_G_NONE) {
        /* v2-owned miss: send nothing (§5), the stroke ends here. */
        ESP_LOGD(TAG, "no region at (%d,%d) gesture %s", x0, y0,
                 p2_gesture_name(g));
        return true;
    }
    const p2_region_t *r = &s_man.regions[idx];

    /* Tier-0 nav resolves its target through the region or the links
     * graph, paints from SD, and reports the new position. A miss (no
     * bundle, expired ttl, cache gone) degrades this activation to
     * tier 2: echo + report + frame poll. */
    if (r->type == P2_ACT_NAV && r->tier == 0) {
        const char *target = r->target[0] ? r->target : NULL;
        if (!target && s_cur_page[0])
            target = p2_bundle_link(&s_bundle, s_cur_page, r->id);
        if (!target && s_cur_page[0])
            target = p2_bundle_link(&s_bundle, s_cur_page, p2_gesture_name(g));
        if (nav_from_bundle(target)) {
            p2_report_t rep;
            memset(&rep, 0, sizeof rep);
            snprintf(rep.region_id, sizeof rep.region_id, "%s", r->id);
            snprintf(rep.gesture, sizeof rep.gesture, "%s", p2_gesture_name(g));
            rep.value = -1;
            snprintf(rep.digest, sizeof rep.digest, "%s", glass);
            rep.event_id = app_next_event_id();
            rep.x0 = x0; rep.y0 = y0; rep.x1 = x1; rep.y1 = y1;
            report_or_queue(&rep);
            return true;   /* no frame poll: the glass already moved */
        }
    }

    /* 1. Local feedback first -- before, and regardless of, the report. */
    bool painted = apply_feedback(idx, r);
    if (painted && r->tier != 0) ledger_add(r);
    /* (tier 0 expects no correction; nothing to track) */

    /* 2. Report (async in spirit: one bounded attempt, then the queue). */
    p2_report_t rep;
    memset(&rep, 0, sizeof rep);
    snprintf(rep.region_id, sizeof rep.region_id, "%s", r->id);
    snprintf(rep.gesture, sizeof rep.gesture, "%s", p2_gesture_name(g));
    rep.value = (g == P2_G_SLIDE) ? value : -1;
    snprintf(rep.digest, sizeof rep.digest, "%s", glass);
    rep.event_id = app_next_event_id();
    rep.x0 = x0; rep.y0 = y0; rep.x1 = x1; rep.y1 = y1;
    report_or_queue(&rep);

    /* Slider: update the declared value_text locally so the number tracks
     * the thumb before any server round trip (format approximated as the
     * bare percentage; the server's next values envelope makes it exact). */
    if (g == P2_G_SLIDE && r->fb == P2_FB_SLIDER && r->fb_value_text[0]) {
        for (int i = 0; i < s_man.n_text; i++)
            if (strcmp(s_man.text[i].id, r->fb_value_text) == 0) {
                char v[8];
                snprintf(v, sizeof v, "%d%%", value);
                draw_text_region(&s_man.text[i], v);
                break;
            }
    }

    /* 3. What happens next. Tier 2 (and nav until bundles land) waits on a
     * new frame; tier 0/1 corrections ride the linger's patch polling. */
    bool nav_like = r->type == P2_ACT_NAV || r->type == P2_ACT_REFRESH ||
                    r->type == P2_ACT_FETCH_LATEST;
    if (want_frame_poll) *want_frame_poll = (r->tier == 2) || nav_like;

    ESP_LOGI(TAG, "%s '%s' tier %d%s (event %llu)", p2_gesture_name(g),
             r->id, r->tier, rep.value >= 0 ? " +value" : "",
             (unsigned long long)rep.event_id);
    return true;
}

#endif /* BOARD_OVERLAY_PARTIAL && BOARD_HAS_TOUCH */
