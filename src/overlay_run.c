/* overlay_run.c -- device orchestration for the overlay render mode. */

#include "overlay_run.h"

#if defined(BOARD_OVERLAY_PARTIAL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "deck.h"          /* deck_sha256 / deck_digest_hex16 (mbedTLS-backed) */
#include "epd_driver.h"
#include "panel/epd_panel.h"   /* epd_active_driver()->info.bpp */
#include "image_fetcher.h"
#include "net_rest.h"
#include "overlay.h"
#include "rest_config.h"
#include "sdcard.h"

static const char *TAG = "overlay";

#define OVERLAY_SPEC_MAX   (8 * 1024)
#define OVERLAY_ATLAS_MAX  (64 * 1024)
#define OVERLAY_DATA_MAX   (4 * 1024)   /* /frame/data body: values + patches */
#define OVERLAY_POLL_MS    1000         /* schema-2 contract: ~1 s in linger */

/* One overlay state per wake. s_base/s_work are full-frame PSRAM copies:
 * base = pristine server frame (slot background restore), work = base +
 * echoes + slot values (what partial refreshes stream from). On a wake that
 * never painted a full frame, s_sparse marks a work buffer reconstructed
 * from SD rect patches only -- valid ONLY inside declared rects. */
static overlay_spec_t s_spec;
static bool     s_have_spec;
static uint8_t *s_atlas_bits[OVERLAY_MAX_ATLASES];
static uint8_t *s_base, *s_work;
static bool     s_sparse;
static char    *s_raw_spec;          /* raw JSON kept for the SD cache */
static size_t   s_raw_spec_len;
static int64_t  s_values_seq = -1;

/* Patch high-water mark (schema 2). Deliberately a PLAIN static, not RTC:
 * the contract forbids persisting a seq across our own reboots (the server's
 * seqs are time-derived and strictly increasing; a fresh boot must accept
 * whatever is current). Newest wins; equal = already applied = no-op. */
static int64_t  s_patch_seq = -1;

/* Set when a patch document could not be honoured (digest mismatch, or no
 * in-RAM frame to composite into). The linger loop consumes it and runs a
 * normal /frame poll -- the contract's only sanctioned fallback. */
static bool     s_refetch;

/* Hygiene counter survives deep sleep so repeated wake-echoes on the same
 * frame still trigger a full-quality pass. */
RTC_DATA_ATTR static overlay_hygiene_t s_hygiene;

static void drop_state(void)
{
    for (int i = 0; i < OVERLAY_MAX_ATLASES; i++) {
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = NULL;
    }
    free(s_raw_spec); s_raw_spec = NULL; s_raw_spec_len = 0;
    memset(&s_spec, 0, sizeof s_spec);
    s_have_spec = false;
    s_values_seq = -1;
    /* base/work stay allocated (reused); s_sparse reset on next fill */
}

static uint8_t *fb_alloc(void)
{
    uint8_t *p = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) ESP_LOGW(TAG, "OOM framebuffer copy; overlay dormant");
    return p;
}

/* ---------- SD cache: /tesserae/overlay/<digest>.spec|.pat, atl/<digest>.bin */

static bool ov_path(char *out, size_t cap, const char *digest, const char *ext)
{
    if (!sdcard_mounted() || !deck_digest_valid(digest)) return false;
    int n = snprintf(out, cap, SDCARD_MOUNT_POINT "/tesserae/overlay/%s.%s",
                     digest, ext);
    return n > 0 && n < (int)cap;
}

static bool atlas_path(char *out, size_t cap, const char *digest)
{
    if (!sdcard_mounted() || !deck_digest_valid(digest)) return false;
    int n = snprintf(out, cap, SDCARD_MOUNT_POINT "/tesserae/overlay/atl/%s.bin",
                     digest);
    return n > 0 && n < (int)cap;
}

static void ensure_dirs(void)
{
    mkdir(SDCARD_MOUNT_POINT "/tesserae", 0775);
    mkdir(SDCARD_MOUNT_POINT "/tesserae/overlay", 0775);
    mkdir(SDCARD_MOUNT_POINT "/tesserae/overlay/atl", 0775);
}

static uint8_t *read_file(const char *path, size_t max, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    /* Internal RAM keeps the SD driver on its fast DMA multi-sector path
     * (PSRAM degrades to single-sector; see deck_cache.c bounce note).
     * Overlay files are small (<=64 KB), so internal is fine. */
    uint8_t *buf = heap_caps_malloc(max, MALLOC_CAP_DMA);
    if (!buf) buf = heap_caps_malloc(max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, max, f);
    bool complete = feof(f);
    fclose(f);
    if (!complete || n == 0) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    ensure_dirs();
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

/* Digest-verify an atlas blob against the spec's declared 16-hex digest. */
static bool atlas_digest_ok(const uint8_t *bits, size_t len, const char *digest)
{
    uint8_t sha[32];
    char hex[DECK_DIGEST_HEX + 1];
    deck_sha256(bits, len, sha);
    deck_digest_hex16(sha, hex);
    return strcmp(hex, digest) == 0;
}

/* Expected packed byte length of an atlas strip. */
static size_t atlas_expected_len(const overlay_atlas_t *a)
{
    size_t row = (a->bpp == 4) ? ((size_t)a->strip_w + 1) / 2
                               : ((size_t)a->strip_w + 7) / 8;
    return row * (size_t)a->height;
}

/* Attach bits for every atlas: SD cache first, network second. False if any
 * atlas is unusable (spec goes dormant). Atlas strip rows must be packed at
 * strip_w exactly (the parse derived strip_w from the glyph table). */
static bool load_atlases(void)
{
    for (int i = 0; i < s_spec.n_atlases; i++) {
        overlay_atlas_t *a = &s_spec.atlases[i];
        size_t want = atlas_expected_len(a);
        if (want == 0 || want > OVERLAY_ATLAS_MAX) return false;

        char path[128];
        size_t got = 0;
        uint8_t *bits = NULL;
        if (atlas_path(path, sizeof path, a->digest))
            bits = read_file(path, want + 1, &got);
        if (bits && (got != want || !atlas_digest_ok(bits, got, a->digest))) {
            ESP_LOGW(TAG, "cached atlas %s bad; deleting", a->digest);
            free(bits); bits = NULL;
            unlink(path);
        }
        if (!bits) {
            char url[320];
            /* Spec URLs may be server-relative. */
            if (a->url[0] == '/')
                snprintf(url, sizeof url, "%s%s", rest_config_get()->server_url, a->url);
            else
                snprintf(url, sizeof url, "%s", a->url);
            fetched_image_t img;
            if (image_fetch_auth(url, rest_bearer_token(), &img) != ESP_OK)
                return false;
            if (img.len != want || !atlas_digest_ok(img.data, img.len, a->digest)) {
                ESP_LOGW(TAG, "atlas %s wrong size/digest", a->digest);
                free(img.data);
                return false;
            }
            bits = img.data;
            if (atlas_path(path, sizeof path, a->digest))
                write_file(path, bits, want);   /* immutable, cache forever */
        }
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = bits;
        a->bits = bits;
    }
    return true;
}

/* ---------- rect patches (offline slot redraw) ----------
 * <frame_digest>.pat: for each SLOT rect only, the byte-aligned window rows
 * (x widened to 4 px) copied straight from the framebuffer, in spec order.
 *
 * Targets are deliberately NOT patched (cap-raise task, 2026-07-24): a dense
 * grid of 32 whole-cell targets would make the patch file approach the full
 * frame size (megabytes on the E1003, seconds of SD write per paint).
 * Invert needs no stored pixels while a frame copy exists in RAM -- it is
 * self-inverse and the next full paint restores it anyway. The trade-off,
 * FLAGGED in the task report: after a deep-sleep wake there is no pixel
 * source for target rects on this hardware (the glass is not readable and
 * the IT8951's DRAM loses power in sleep), so cold-wake taps skip the local
 * echo and fall through to today's server round trip. Slots stay patched
 * (max 8, small rects) so status-borne values can still redraw offline. */

#define EPD_BPP_NUM (epd_active_driver()->info.bpp)

static void rect_window(int x, int w, int *bx0, int *bx1)
{
    int x0 = x & ~3, x1 = (x + w + 3) & ~3;
    if (x1 > EPD_WIDTH) x1 = EPD_WIDTH;
    *bx0 = x0 * EPD_BPP_NUM / 8;   /* byte offsets */
    *bx1 = x1 * EPD_BPP_NUM / 8;
}

static size_t patch_size(void)
{
    size_t total = 0;
    for (int i = 0; i < s_spec.n_slots; i++) {
        const overlay_slot_t *sl = &s_spec.slots[i];
        int b0, b1;
        rect_window(sl->x, sl->w, &b0, &b1);
        total += (size_t)(b1 - b0) * sl->h;
    }
    return total;
}

static void patch_copy(uint8_t *dst_file, const uint8_t *fb, bool to_file)
{
    const int pitch = EPD_BUF_BYTES / EPD_HEIGHT;
    size_t off = 0;
    for (int i = 0; i < s_spec.n_slots; i++) {
        const overlay_slot_t *sl = &s_spec.slots[i];
        int b0, b1;
        rect_window(sl->x, sl->w, &b0, &b1);
        for (int yy = sl->y; yy < sl->y + sl->h; yy++) {
            uint8_t *fbp = (uint8_t *)fb + (size_t)yy * pitch + b0;
            if (to_file) memcpy(dst_file + off, fbp, b1 - b0);
            else         memcpy(fbp, dst_file + off, b1 - b0);
            off += (size_t)(b1 - b0);
        }
    }
}

/* ---------- spec acquisition ---------- */

static bool spec_usable_for(const char *digest)
{
    return s_have_spec && overlay_spec_matches(&s_spec, digest);
}

void overlay_frame_downloaded(const char *digest)
{
    if (!epd_supports_partial() || !digest || !digest[0]) return;
    drop_state();

    char *buf = heap_caps_malloc(OVERLAY_SPEC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    size_t len = 0;
    rest_status_t st = rest_get_overlay_spec(digest, buf, OVERLAY_SPEC_MAX, &len, 8000);
    if (st != REST_OK) {
        if (st != REST_NOT_FOUND)
            ESP_LOGW(TAG, "overlay spec fetch failed (%d)", st);
        free(buf);
        return;   /* 404 = feature off for this frame; dormant */
    }
    if (!overlay_spec_parse(buf, len, EPD_WIDTH, EPD_HEIGHT, &s_spec) ||
        !overlay_spec_matches(&s_spec, digest)) {
        ESP_LOGW(TAG, "overlay spec unusable; dormant for this frame");
        free(buf);
        return;
    }
    if (!load_atlases()) {
        ESP_LOGW(TAG, "atlas load failed; overlay dormant for this frame");
        drop_state();
        free(buf);
        return;
    }
    s_raw_spec = buf;
    s_raw_spec_len = len;
    s_have_spec = true;
    ESP_LOGI(TAG, "overlay ready for %s: %d targets, %d slots, %d atlases",
             digest, s_spec.n_targets, s_spec.n_slots, s_spec.n_atlases);
}

void overlay_after_paint(const uint8_t *frame, const char *digest)
{
    overlay_hygiene_reset(&s_hygiene);   /* full paint clears ghosting */
    if (!spec_usable_for(digest)) return;

    if (!s_base && !(s_base = fb_alloc())) { drop_state(); return; }
    if (!s_work && !(s_work = fb_alloc())) { drop_state(); return; }
    memcpy(s_base, frame, EPD_BUF_BYTES);
    memcpy(s_work, frame, EPD_BUF_BYTES);
    s_sparse = false;

    /* SD cache: raw spec + rect patches, so the next wake can echo offline. */
    if (sdcard_mounted()) {
        char path[128];
        if (ov_path(path, sizeof path, digest, "spec"))
            write_file(path, s_raw_spec, s_raw_spec_len);
        size_t psz = patch_size();
        uint8_t *pat = psz ? malloc(psz) : NULL;
        if (pat) {
            patch_copy(pat, s_base, true);
            if (ov_path(path, sizeof path, digest, "pat"))
                write_file(path, pat, psz);
            free(pat);
        }
    }
}

void overlay_boot(void)
{
    if (!epd_supports_partial() || s_have_spec) return;
    const char *digest = rest_config_get()->last_frame_etag;
    if (!sdcard_mounted() || !deck_digest_valid(digest)) return;

    char path[128];
    size_t len = 0;
    if (!ov_path(path, sizeof path, digest, "spec")) return;
    uint8_t *buf = read_file(path, OVERLAY_SPEC_MAX, &len);
    if (!buf) return;
    if (!overlay_spec_parse((const char *)buf, len, EPD_WIDTH, EPD_HEIGHT, &s_spec) ||
        !overlay_spec_matches(&s_spec, digest)) {
        free(buf);
        memset(&s_spec, 0, sizeof s_spec);
        return;
    }
    s_raw_spec = (char *)buf;
    s_raw_spec_len = len;

    /* Rebuild a sparse work buffer from the rect patches. */
    if (!s_work && !(s_work = fb_alloc())) { drop_state(); return; }
    memset(s_work, 0, EPD_BUF_BYTES);
    size_t psz = patch_size(), got = 0;
    if (psz) {
        if (!ov_path(path, sizeof path, digest, "pat")) { drop_state(); return; }
        uint8_t *pat = read_file(path, psz + 1, &got);
        if (!pat || got != psz) { free(pat); drop_state(); return; }
        patch_copy(pat, s_work, false);
        free(pat);
    }
    s_sparse = true;
    s_have_spec = true;
    ESP_LOGI(TAG, "overlay restored from SD for %s (slot patches only)", digest);
}

/* ---------- echo + slots ---------- */

static void hygiene_or_partial(int x, int y, int w, int h, bool fast)
{
    if (overlay_hygiene_tick(&s_hygiene)) {
        if (!s_sparse && s_work) {
            ESP_LOGI(TAG, "hygiene: full-quality repaint");
            epd_display(s_work);   /* GC16 full clears ghosting */
            overlay_hygiene_reset(&s_hygiene);
            return;
        }
        /* Sparse wake: no full frame on hand. Force the next network cycle
         * to repaint fully by dropping the cached ETag. Persist it: the
         * mutator only touches the RAM cache, and this wake's config save
         * belongs to main -- without an explicit save the old ETag comes
         * back from NVS next wake and the refetch never happens. */
        rest_config_set_frame_etag("");
        rest_config_save();
        overlay_hygiene_reset(&s_hygiene);
    }
    epd_display_partial(s_work, x, y, w, h, fast);
}

bool overlay_try_echo(int x, int y)
{
    if (!s_have_spec || !s_work) return false;
    if (!overlay_spec_matches(&s_spec, rest_config_get()->last_frame_etag))
        return false;   /* staleness rule */
    /* Sparse (cold-wake) buffers only carry slot patches: target rects have
     * no pixel source, so a true invert is impossible -- skip the echo and
     * let the normal dispatch provide the feedback. See the patch comment. */
    if (s_sparse) return false;

    const overlay_target_t *t = overlay_hit_target(&s_spec, x, y);
    if (!t) return false;

    int64_t t0 = esp_timer_get_time();
    overlay_invert_rect(s_work, EPD_WIDTH, EPD_HEIGHT,
                        epd_active_driver()->info.bpp, t->x, t->y, t->w, t->h);
    if (epd_port_init() == ESP_OK) {
        epd_init();
        hygiene_or_partial(t->x, t->y, t->w, t->h, true /* DU */);
    }
    ESP_LOGI(TAG, "echo '%s' in %lld ms (invert %d,%d %dx%d)",
             t->id, (esp_timer_get_time() - t0) / 1000, t->x, t->y, t->w, t->h);
    return true;
}

static void redraw_changed_slots(uint32_t changed)
{
    if (!changed || !s_work) return;
    /* Slot redraw needs the pristine background: on a sparse buffer the
     * patches ARE the pristine background inside declared rects. */
    const uint8_t *base = s_sparse ? s_work : s_base;
    if (!base) return;

    for (int i = 0; i < s_spec.n_slots; i++) {
        if (!(changed & (1u << i))) continue;
        overlay_slot_t *sl = &s_spec.slots[i];
        overlay_atlas_t *a = overlay_find_atlas(&s_spec, sl->atlas_id);
        int64_t t0 = esp_timer_get_time();
        overlay_draw_slot(s_work, base, EPD_WIDTH, EPD_HEIGHT,
                          epd_active_driver()->info.bpp, sl, a);
        hygiene_or_partial(sl->x, sl->y, sl->w, sl->h, true);
        ESP_LOGI(TAG, "slot '%s' = \"%s\" in %lld ms", sl->id, sl->value,
                 (esp_timer_get_time() - t0) / 1000);
    }
}

void overlay_ingest_values(const char *json, size_t len)
{
    if (!s_have_spec || !json || !len) return;
    uint32_t changed = overlay_values_apply(&s_spec, json, len, &s_values_seq);
    if (changed && epd_port_init() == ESP_OK) {
        epd_init();
        redraw_changed_slots(changed);
    }
}

/* ---------- patch documents (schema 2: post-action frame patches) ---------- */

void overlay_ingest_patches(const char *json, size_t len)
{
    if (!json || !len || !epd_supports_partial()) return;

    /* Static: called deep in the linger loop's call chain; ~800 B of doc +
     * url on the main task stack contributed to a bench stack overflow.
     * Single-threaded per wake, so a static is safe. */
    static overlay_patch_doc_t doc;
    if (!overlay_patch_parse(json, len, EPD_WIDTH, EPD_HEIGHT, EPD_BPP_NUM, &doc))
        return;   /* malformed = dropped whole; never clipped or best-efforted */

    /* Digest anchor (hard rule): the document must name the frame currently
     * on glass. A mismatch means one side moved on -- drop the document and
     * let a normal /frame poll converge (server returns 200 when it really
     * has a newer frame, 304 when the doc was the stale party). */
    const char *glass = rest_config_get()->last_frame_etag;
    if (strcmp(doc.frame_digest, glass) != 0) {
        ESP_LOGW(TAG, "patch doc for %s but glass shows %s; dropping",
                 doc.frame_digest, glass[0] ? glass : "(none)");
        s_refetch = true;
        return;
    }

    /* Newest wins; equal seq = the doc we already applied. */
    if (doc.seq <= s_patch_seq) return;

    /* Patches composite into the in-RAM frame copy. Without a full one
     * (cold wake: none, or SD-restored sparse slot patches only) there is
     * nothing correct to stream partial refreshes from -- and since the
     * digest never changes under schema 2, waiting on a 304 would show
     * stale glass forever. Clear the ETag so the fallback poll re-downloads
     * the full frame (the server's frame body already reflects the patch). */
    if (!s_work || s_sparse) {
        /* No pixel source to composite into. Only flag the refetch: the
         * linger loop consumes it (someone is watching; worth a full
         * repaint). On a timer wake the flag simply expires -- the server
         * stages patches for every periodic re-render (clock/sensor rects),
         * and paying a 30 s GC16 per clock tick would burn the panel and
         * the battery; values keep the declared slots fresh regardless. */
        ESP_LOGI(TAG, "patch %lld with no full frame in RAM; linger may refetch",
                 (long long)doc.seq);
        s_refetch = true;
        return;
    }

    /* Fetch the blob (Bearer; immutable for its lifetime, <= 256 KB so the
     * linger loop is back polling touch within the contract's ~1.5 s). A
     * 404 means a newer action superseded it: drop, the next 1 s poll
     * carries the newer document. Any other failure: retry on next poll. */
    char url[320];
    if (doc.url[0] == '/')
        snprintf(url, sizeof url, "%s%s", rest_config_get()->server_url, doc.url);
    else
        snprintf(url, sizeof url, "%s", doc.url);
    fetched_image_t img;
    esp_err_t err = image_fetch_auth(url, rest_bearer_token(), &img);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND)
            ESP_LOGI(TAG, "patch blob seq %lld superseded (404)",
                     (long long)doc.seq);
        return;
    }
    if (img.len != doc.bytes) {
        ESP_LOGW(TAG, "patch blob %u B != declared %u B; dropping",
                 (unsigned)img.len, (unsigned)doc.bytes);
        free(img.data);
        return;
    }

    /* Apply every rect to BOTH copies: base (pristine background future slot
     * redraws restore from) and work (what partials stream from). Writing
     * over a pending tap-echo inversion is the point -- the patch IS the
     * action's result and supersedes the echo; never re-invert. */
    int64_t t0 = esp_timer_get_time();
    bool applied = true;
    for (int i = 0; i < doc.n_rects; i++) {
        applied &= overlay_patch_apply_rect(s_work, EPD_WIDTH, EPD_HEIGHT,
                                            EPD_BPP_NUM, &doc.rects[i],
                                            img.data, img.len);
        if (s_base)
            overlay_patch_apply_rect(s_base, EPD_WIDTH, EPD_HEIGHT,
                                     EPD_BPP_NUM, &doc.rects[i],
                                     img.data, img.len);
    }
    free(img.data);
    if (!applied) {   /* blob-bounds refusal: buffers may be half-patched */
        rest_config_set_frame_etag("");
        rest_config_save();
        s_refetch = true;
        return;
    }

    /* Slot values ride their own seq stream: a slot whose rect the patch
     * touched gets redrawn on top of the fresh base so the newest value
     * string wins over an older render baked into the patch. */
    uint32_t slots_hit = 0;
    if (s_have_spec && overlay_spec_matches(&s_spec, glass)) {
        for (int si = 0; si < s_spec.n_slots; si++) {
            const overlay_slot_t *sl = &s_spec.slots[si];
            for (int i = 0; i < doc.n_rects; i++)
                if (overlay_patch_rect_intersects(&doc.rects[i],
                                                  sl->x, sl->y, sl->w, sl->h)) {
                    slots_hit |= 1u << si;
                    break;
                }
        }
    }

    if (epd_port_init() == ESP_OK) {
        epd_init();
        redraw_changed_slots(slots_hit);   /* draws + refreshes slot rects */
        /* One refresh over the union bounding box, not one per rect: at
         * ~1 s of DU waveform per window, 8-10 rects serialized cost 10+ s
         * and trip the hygiene limit mid-patch (bench 2026-07-25: 16-22 s
         * per apply). The buffers already hold every rect, so a single
         * bbox pass paints them all in one waveform and one hygiene tick. */
        int bx0 = doc.rects[0].x, by0 = doc.rects[0].y;
        int bx1 = bx0 + doc.rects[0].w, by1 = by0 + doc.rects[0].h;
        for (int i = 1; i < doc.n_rects; i++) {
            const overlay_patch_rect_t *r = &doc.rects[i];
            if (r->x < bx0) bx0 = r->x;
            if (r->y < by0) by0 = r->y;
            if (r->x + r->w > bx1) bx1 = r->x + r->w;
            if (r->y + r->h > by1) by1 = r->y + r->h;
        }
        hygiene_or_partial(bx0, by0, bx1 - bx0, by1 - by0, true /* DU */);
    }
    s_patch_seq = doc.seq;
    ESP_LOGI(TAG, "patch seq %lld applied: %d rect(s), %u B in %lld ms",
             (long long)doc.seq, doc.n_rects, (unsigned)doc.bytes,
             (esp_timer_get_time() - t0) / 1000);
}

bool overlay_take_refetch(void)
{
    bool r = s_refetch;
    s_refetch = false;
    return r;
}

void overlay_linger_poll(void)
{
    static int64_t s_last_poll;
    /* Schema 2: patches can land for ANY frame after a touch action, spec or
     * no spec, so the gate is only "a digest is on glass". */
    const char *digest = rest_config_get()->last_frame_etag;
    if (!deck_digest_valid(digest)) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_poll < (int64_t)OVERLAY_POLL_MS * 1000) return;
    s_last_poll = now;

    char *buf = heap_caps_malloc(OVERLAY_DATA_MAX, MALLOC_CAP_8BIT);
    if (!buf) return;
    size_t len = 0;
    rest_status_t st = rest_get_frame_data(digest, buf, OVERLAY_DATA_MAX,
                                           &len, 3000);
    if (st == REST_OK) {         /* incl. 404: silently dormant */
        overlay_ingest_values(buf, len);
        overlay_ingest_patches(buf, len);
    }
    free(buf);
}

#endif /* BOARD_OVERLAY_PARTIAL */
