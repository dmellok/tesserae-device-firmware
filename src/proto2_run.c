/* proto2_run.c -- device orchestration for protocol v2. See proto2_run.h. */

#include "proto2_run.h"

#if defined(BOARD_OVERLAY_PARTIAL) && defined(BOARD_HAS_TOUCH)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "deck.h"           /* deck_digest_valid (16-hex check) */
#include "epd_driver.h"
#include "panel/epd_panel.h"   /* epd_active_driver()->info.bpp */
#include "net_rest.h"
#include "overlay.h"        /* overlay_invert_rect (shared fb primitive) */
#include "overlay_run.h"    /* framebuffer + hygiene accessors */
#include "proto2.h"
#include "rest_config.h"
#include "sdcard.h"

static const char *TAG = "proto2";

#define P2_MANIFEST_MAX   (16 * 1024)   /* raw JSON cap (32 KB parsed pools) */
#define P2_LEDGER_MAX     16
#define P2_RQUEUE_MAX     16

/* Minted by main.c (the single wake-event counter, uint64, <= 2^53). */
extern uint64_t app_next_event_id(void);

/* ---- state (statics: the linger call chain must stay off the stack) ---- */

static p2_manifest_t s_man;
static bool          s_have_man;
static char         *s_raw_man;         /* raw JSON kept for the SD cache */
static size_t        s_raw_man_len;

/* Server generation probe, cached per boot (a wake = a connection epoch):
 * 0 unknown, +1 v2 (manifest block seen), -1 v1 (200 without the block). */
static int s_epoch;

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
    (void)digest;
    ledger_clear_all();   /* a full frame covers every optimistic rect */
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

/* ---- manifest lifecycle ---- */

static void drop_manifest(void)
{
    free(s_raw_man);
    s_raw_man = NULL;
    s_raw_man_len = 0;
    s_have_man = false;
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

/* Apply the region's feedback into the work framebuffer + partial refresh.
 * Stage C renders invert for every mode (tiles/slider visuals land with the
 * bundle + text stages); "none" stays dark. Returns true when pixels moved
 * (the caller then ledgers them). */
static bool apply_feedback(const p2_region_t *r)
{
    if (r->fb == P2_FB_NONE) return false;
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

    /* 1. Local feedback first -- before, and regardless of, the report. */
    bool painted = apply_feedback(r);
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
