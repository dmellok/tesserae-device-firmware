/* touch3_run.c -- device orchestration for touch v3. See touch3_run.h. */

#include "touch3_run.h"
#include "app_config.h"

#if BOARD_TOUCH3

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp (digest compare) */
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"   /* PSRAM feedback scratch buffer */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"      /* vTaskDelay: hold the press affordance */
#include "image_fetcher.h"

#include "deck.h"              /* deck_sha256 / deck_digest_hex16 */
#include "epd_driver.h"
#include "net_rest.h"
#include "overlay.h"           /* overlay_invert_rect (shared fb primitive) */
#include "overlay_run.h"       /* work framebuffer + partial refresh */
#include "panel/epd_panel.h"   /* epd_active_driver()->info.bpp */
#include "rest_config.h"
#include "sdcard.h"
#include "touch3.h"

static const char *TAG = "touch3";

/* Raw spec JSON cap. A real page ran 46 primitives (~250 B each) and two atlas
 * descriptors carry ~3 KB of glyph map apiece, so 64 primitives plus atlases
 * needs real headroom. Transient malloc, freed straight after the parse.
 *
 * NOTE this cannot exceed net_rest's REST_RX_MAX in any useful way: small_get()
 * copies out of that shared static buffer, so REST_RX_MAX is the true ceiling
 * and both must be raised together. Kept equal so the two cannot drift. */
#define T3_SPEC_MAX     (32 * 1024)
#define T3_ATLAS_MAX_B  (64 * 1024)   /* per-strip cap */
#define T3_RQUEUE_MAX   16
#define T3_DRAG_MIN_US  (350 * 1000)  /* min gap between live-drag DU refreshes */
#define T3_DRAG_IDLE_US (2000 * 1000) /* older than this = a new stroke */

#define T3_DIR          SDCARD_MOUNT_POINT "/tesserae/touch3"

/* Minted by main.c (the single wake-event counter, uint64, <= 2^53). */
extern uint64_t app_next_event_id(void);

/* ---- state (statics: the linger call chain must stay off the stack) ---- */

static t3_spec_t s_spec;
static bool      s_have_spec;
static uint8_t  *s_atlas_bits[T3_MAX_ATLASES];
static char      s_glass_layout[T3_DIGEST_CAP];   /* layout on glass */

/* Live-drag tracking across touch_capture_stroke_cb() samples. s_drag_armed
 * marks "this stroke's primitive is already latched", so the hit test runs
 * once per stroke rather than once per 8 ms sample; s_drag_idx is the slider
 * being dragged, or -1 when this stroke tracks nothing. */
static int64_t  s_values_seq = -1;    /* values stream high-water mark */

/* Set when a spec pull goes from "no controls" to "some controls". The frame on
 * glass was composed without them, so it has to be repainted to show them --
 * see touch3_take_repaint(). */
static bool     s_want_repaint;

static bool     s_drag_armed;
static int      s_drag_idx = -1;
static int64_t  s_drag_last_us;
static int64_t  s_drag_seen_us;

/* /interact report queue: replayed in order; event-id dedup makes retries
 * safe. RAM-only, same reasoning as the v2 queue (a report still pending at
 * sleep is logged and dropped). */
typedef struct {
    char     primitive_id[T3_ID_CAP];
    char     interaction[8];
    bool     has_value;
    float    value;
    char     layout_digest[T3_DIGEST_CAP];
    uint64_t event_id;
} t3_report_t;
static t3_report_t s_rq[T3_RQUEUE_MAX];
static int s_rq_head, s_rq_len;

static int panel_bpp(void) { return epd_active_driver()->info.bpp; }

const char *touch3_layout_digest(void)
{
    return s_have_spec ? s_spec.layout_digest : "";
}

bool touch3_active(void)
{
    return s_have_spec && s_spec.n_prims > 0 &&
           t3_spec_matches(&s_spec, s_glass_layout);
}

/* ---- SD cache --------------------------------------------------------- */

/* A digest is only used to build a filename after this check: hex-ish, short,
 * no separators. layout_digest is 12 hex in the contract examples, so the
 * 16-hex deck_digest_valid() rule is too narrow here. */
static bool digest_safe(const char *d)
{
    if (!d || !d[0]) return false;
    size_t n = strlen(d);
    if (n >= T3_DIGEST_CAP) return false;
    for (size_t i = 0; i < n; i++) {
        char c = d[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static void ensure_dirs(void)
{
    mkdir(SDCARD_MOUNT_POINT "/tesserae", 0775);
    mkdir(T3_DIR, 0775);
}

static bool spec_path(char *out, size_t cap, const char *layout)
{
    if (!sdcard_mounted() || !digest_safe(layout)) return false;
    int n = snprintf(out, cap, T3_DIR "/%s.spec", layout);
    return n > 0 && n < (int)cap;
}

static bool atlas_path(char *out, size_t cap, const char *digest)
{
    if (!sdcard_mounted() || !digest_safe(digest)) return false;
    int n = snprintf(out, cap, T3_DIR "/atlas-%s.bin", digest);
    return n > 0 && n < (int)cap;
}

/* The layout on glass, so the next boot restores the right spec before the
 * radio comes up. */
static void write_current(const char *layout)
{
    if (!sdcard_mounted() || !digest_safe(layout)) return;
    ensure_dirs();
    FILE *f = fopen(T3_DIR "/current", "wb");
    if (!f) return;
    fwrite(layout, 1, strlen(layout), f);
    fclose(f);
}

static bool read_current(char *out, size_t cap)
{
    out[0] = '\0';
    if (!sdcard_mounted()) return false;
    FILE *f = fopen(T3_DIR "/current", "rb");
    if (!f) return false;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return digest_safe(out);
}

/* ---- atlas strips (SD first, network second; PSRAM resident) ---------- */

static size_t atlas_expected_len(const t3_atlas_t *a)
{
    return (size_t)((a->strip_w + 1) / 2) * (size_t)a->strip_h;
}

/* The contract calls the atlas digest a content hash without pinning its
 * length. Verify it when it is the house 16-hex sha256 prefix; otherwise fall
 * back to the length check the descriptor already implies, and say so once. */
static bool atlas_verify(const uint8_t *bits, size_t len, const char *digest)
{
    if (strlen(digest) != DECK_DIGEST_HEX) {
        static bool warned;
        if (!warned) {
            ESP_LOGW(TAG, "atlas digest '%s' is not 16-hex; length-checking "
                          "only", digest);
            warned = true;
        }
        return true;
    }
    uint8_t sha[32];
    char hex[DECK_DIGEST_HEX + 1];
    deck_sha256(bits, len, sha);
    deck_digest_hex16(sha, hex);
    return strcasecmp(hex, digest) == 0;
}

static void free_atlases(void)
{
    for (int i = 0; i < T3_MAX_ATLASES; i++) {
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = NULL;
    }
}

/* Attach bits for every atlas the spec references. allow_net=false during the
 * boot restore (radio down). A text ref whose atlas fails stays parsed but
 * unrenderable: t3_draw_text no-ops, so the control keeps its chrome and only
 * loses its label (firmware-spec §13 -- never a blank control). */
static void load_atlases(bool allow_net)
{
    for (int i = 0; i < s_spec.n_atlases; i++) {
        t3_atlas_t *a = &s_spec.atlases[i];
        if (a->bits) continue;
        size_t want = atlas_expected_len(a);
        if (want == 0 || want > T3_ATLAS_MAX_B) {
            ESP_LOGW(TAG, "atlas %s implausible size %u", a->id,
                     (unsigned)want);
            continue;
        }

        char path[160];
        uint8_t *bits = NULL;
        if (atlas_path(path, sizeof path, a->digest)) {
            FILE *f = fopen(path, "rb");
            if (f) {
                bits = malloc(want + 1);
                size_t got = bits ? fread(bits, 1, want + 1, f) : 0;
                fclose(f);
                if (!bits || got != want || !atlas_verify(bits, got, a->digest)) {
                    free(bits); bits = NULL;
                    unlink(path);            /* corrupt: drop the cache entry */
                }
            }
        }
        if (!bits && allow_net) {
            char url[320];
            if (a->url[0] == '/')
                snprintf(url, sizeof url, "%s%s",
                         rest_config_get()->server_url, a->url);
            else
                snprintf(url, sizeof url, "%s", a->url);
            fetched_image_t img;
            if (image_fetch_auth(url, rest_bearer_token(), &img) != ESP_OK) {
                ESP_LOGW(TAG, "atlas %s fetch failed", a->id);
                continue;
            }
            if (img.len != want || !atlas_verify(img.data, img.len, a->digest)) {
                ESP_LOGW(TAG, "atlas %s wrong size/digest (%u want %u)", a->id,
                         (unsigned)img.len, (unsigned)want);
                free(img.data);
                continue;
            }
            bits = img.data;
            if (atlas_path(path, sizeof path, a->digest)) {
                ensure_dirs();
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(bits, 1, want, f); fclose(f); }
            }
        }
        if (!bits) continue;
        free(s_atlas_bits[i]);
        s_atlas_bits[i] = bits;
        a->bits = bits;
    }
}

/* ---- spec lifecycle --------------------------------------------------- */

static void drop_spec(void)
{
    free_atlases();
    memset(&s_spec, 0, sizeof s_spec);
    s_have_spec = false;
    s_drag_armed = false;
    s_drag_idx = -1;
}

/* Parse a raw spec body and load its atlases. Caches the raw JSON on SD when
 * `cache` (a fresh network fetch), skipping it on a restore from that cache.
 *
 * The RETURNED layout_digest is authoritative: ?layout= is advisory and the
 * server answers for the device's current frame regardless (server sync
 * 2026-07-27), so a layout change is detected here, by comparing what came back
 * against what is held. */
static bool adopt_spec(const char *json, size_t len, bool cache, bool allow_net)
{
    /* STATIC, never a stack local. t3_spec_t is ~19 KB at T3_MAX_PRIMS=64 and
     * the main task stack is 16 KB, so declaring this on the stack overflowed
     * it -- and not cleanly: the overflow scribbled over newlib's stdio lock,
     * after which the next printf spun forever in spinlock_acquire and the
     * INTERRUPT watchdog fired. The backtrace pointed at an innocent ESP_LOGI,
     * which made it look like a logging bug rather than a stack bug.
     * adopt_spec is only ever called from the single wake/boot path, so a
     * static scratch is safe and keeps the cost off the stack entirely. */
    static t3_spec_t parsed;
    if (!t3_spec_parse(json, len, EPD_WIDTH, EPD_HEIGHT, &parsed)) {
        ESP_LOGW(TAG, "spec parse failed (%u bytes)", (unsigned)len);
        return false;
    }

    /* Same layout as held: keep the parsed spec AND its loaded atlas strips
     * rather than re-adopting, which would drop and refetch every strip on each
     * frame poll. Seeded state/value ARE data and can move under an unchanged
     * layout_digest, so adopt those. */
    if (s_have_spec && t3_spec_matches(&s_spec, parsed.layout_digest)) {
        for (int i = 0; i < parsed.n_prims; i++) {
            t3_prim_t *held = t3_prim_by_id(&s_spec, parsed.prims[i].id);
            if (!held) continue;
            held->state = parsed.prims[i].state;
            held->value = parsed.prims[i].value;
            held->optimistic = false;   /* a fresh frame is the server's truth */
        }
        load_atlases(allow_net);        /* retry any strip that failed earlier */
        return true;
    }

    /* Gaining controls where there were none means the frame already on glass
     * was composed without them: flag a repaint so they actually appear. */
    bool had_controls = s_have_spec && s_spec.n_prims > 0;
    if (!had_controls && parsed.n_prims > 0) s_want_repaint = true;

    drop_spec();
    s_spec = parsed;
    s_have_spec = true;
    load_atlases(allow_net);

    if (cache) {
        char path[160];
        if (spec_path(path, sizeof path, s_spec.layout_digest)) {
            ensure_dirs();
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(json, 1, len, f); fclose(f); }
        }
    }
    if (s_spec.n_prims == 0) {
        ESP_LOGI(TAG, "spec %s is empty (feature off / non-touch page); "
                      "holding no controls", s_spec.layout_digest);
    } else {
        int nb = 0, nw = 0, nsl = 0, nst = 0;
        for (int i = 0; i < s_spec.n_prims; i++)
            switch (s_spec.prims[i].type) {
            case T3_BUTTON:  nb++;  break;
            case T3_SWITCH:  nw++;  break;
            case T3_SLIDER:  nsl++; break;
            case T3_STEPPER: nst++; break;
            }
        /* Per-type counts, because a whole type silently vanishing is exactly
         * the failure mode that hid the missing-action bug: sliders and
         * steppers parsed and were then dropped, and a bare total looked
         * plausible. */
        ESP_LOGI(TAG, "spec %s: %d primitive(s) [%d btn, %d sw, %d slider, "
                      "%d stepper], %d atlas(es)%s",
                 s_spec.layout_digest, s_spec.n_prims, nb, nw, nsl, nst,
                 s_spec.n_atlases,
                 s_spec.n_atlases ? "" : " -- no atlases: chrome only, no text");
        if (s_spec.n_prims >= T3_MAX_PRIMS)
            ESP_LOGW(TAG, "primitive list hit the cap of %d -- the server may "
                          "have sent more that were TRUNCATED", T3_MAX_PRIMS);
    }
    return true;
}

void touch3_boot(void)
{
    char layout[T3_DIGEST_CAP];
    if (!read_current(layout, sizeof layout)) return;
    char path[160];
    if (!spec_path(path, sizeof path, layout)) return;

    FILE *f = fopen(path, "rb");
    if (!f) return;
    char *buf = malloc(T3_SPEC_MAX);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, T3_SPEC_MAX - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (n && adopt_spec(buf, n, false, false)) {
        snprintf(s_glass_layout, sizeof s_glass_layout, "%s", layout);
        ESP_LOGI(TAG, "restored spec for layout %s from SD", layout);
    }
    free(buf);
}

void touch3_frame_downloaded(const char *frame_digest,
                             const char *layout_digest)
{
    (void)frame_digest;

    /* Fast path: /frame ADVERTISED a layout_digest and it matches the held
     * spec, so this is a data-only redraw -- reuse everything, no request. Only
     * an optimisation: the server does not carry layout_digest on /frame yet,
     * in which case layout_digest is "" and we always pull below. */
    if (layout_digest && layout_digest[0] && s_have_spec &&
        t3_spec_matches(&s_spec, layout_digest)) {
        load_atlases(true);            /* retry any atlas that failed earlier */
        return;
    }

    /* Otherwise pull the spec. ?layout= is ADVISORY (server sync 2026-07-27):
     * the server answers for the device's current frame regardless, and the
     * returned layout_digest is what tells us whether the layout moved. So the
     * pull -- not a signal on /frame -- is what detects v3 at all. Passing the
     * digest we hold lets a future server short-circuit it. A few KB of JSON per
     * frame poll is noise next to the ~1.3 MB frame. */
    /* Only send ?layout= when we hold a spec that actually HAS controls. The
     * param is advisory (the server answers for its current frame regardless
     * and the returned digest is authoritative), so a stale or empty-layout
     * digest buys nothing -- and empirically costs: passing the digest of a
     * previously-empty layout made /frame/spec hang until the client timed out
     * (ESP_ERR_HTTP_EAGAIN at both 5 s and 15 s), while the same endpoint
     * answers 200 when it is not sent. Omitting it removes the coupling. */
    const char *held = (s_have_spec && s_spec.n_prims > 0)
                     ? s_spec.layout_digest : "";
    char *buf = malloc(T3_SPEC_MAX);
    if (!buf) { ESP_LOGW(TAG, "no heap for spec fetch"); return; }
    /* 15 s, not 5: a 46-primitive spec timed out at 5 s on the live server
     * (REST_NET_ERR, 0 bytes, 5.1 s elapsed) -- building it is evidently not
     * instant. This runs once per layout change, not per poll, and a failure
     * just keeps the held spec, so a generous bound costs nothing. */
    size_t len = 0;
    rest_status_t st = rest_get_frame_spec(held, buf, T3_SPEC_MAX, &len, 15000);
    /* One line per fetch, unconditionally: status, bytes, and (once parsed) the
     * layout + primitive count. Without this a silently-dropped primitive list
     * is indistinguishable from "the server sent nothing". */
    ESP_LOGI(TAG, "GET /frame/spec (held '%s') -> st=%d, %u bytes", held, st,
             (unsigned)len);
    if (st == REST_NOT_FOUND || st == REST_NO_CONTENT) {
        /* "No touch for this frame": render the image only and let the v2/v1
         * dispatch take the next touch (firmware-spec §5). */
        if (s_have_spec) ESP_LOGI(TAG, "no spec for this frame (%d); v3 dormant",
                                  st);
        drop_spec();
        s_glass_layout[0] = '\0';
        free(buf);
        return;
    }
    if (st != REST_OK || len == 0) {
        /* Never block the image on a spec failure: keep whatever is held. */
        ESP_LOGW(TAG, "spec fetch failed (%d); keeping any held spec", st);
        free(buf);
        return;
    }
    adopt_spec(buf, len, true, true);
    free(buf);
}

bool touch3_take_repaint(void)
{
    bool w = s_want_repaint;
    s_want_repaint = false;
    return w;
}

void touch3_poll_spec(void)
{
    /* Re-ask even though no new frame arrived. Without this the spec is only
     * ever pulled alongside a frame download, so enabling controls server-side
     * on an UNCHANGED page (a 304 every poll) would never be noticed -- the
     * device would sit on its empty spec indefinitely. Passing no layout digest
     * forces the pull rather than taking the cached fast path. */
    touch3_frame_downloaded("", "");
}

void touch3_compose(uint8_t *fb)
{
    if (!fb || !s_have_spec || s_spec.n_prims == 0) return;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < s_spec.n_prims; i++)
        t3_draw_primitive(fb, EPD_WIDTH, EPD_HEIGHT, panel_bpp(), &s_spec,
                          &s_spec.prims[i]);
    ESP_LOGI(TAG, "composed %d primitive(s) in %lld ms", s_spec.n_prims,
             (esp_timer_get_time() - t0) / 1000);
}

void touch3_after_paint(const char *frame_digest)
{
    (void)frame_digest;
    if (!s_have_spec) return;
    snprintf(s_glass_layout, sizeof s_glass_layout, "%s", s_spec.layout_digest);
    for (int i = 0; i < s_spec.n_prims; i++) {
        s_spec.prims[i].partials = 0;      /* a GC16 just cleared the ghosts */
        s_spec.prims[i].optimistic = false;
    }
    write_current(s_spec.layout_digest);
}

/* ---- reports ---------------------------------------------------------- */

static bool report_send(const t3_report_t *rep, rest_interact_out_t *out)
{
    rest_status_t st = rest_post_interact(rep->primitive_id, rep->interaction,
                                         rep->has_value, rep->value,
                                         rep->layout_digest, rep->event_id,
                                         out, 3000);
    if (st == REST_OK) return true;
    /* 404 = the server does not know this primitive (our spec raced a layout
     * change); retrying cannot help, and 401 is re-pair territory. */
    if (st == REST_NOT_FOUND || st == REST_UNAUTH) {
        ESP_LOGI(TAG, "'%s' not accepted (%d); dropping report",
                 rep->primitive_id, st);
        return true;
    }
    return false;
}

static void rq_push(const t3_report_t *rep)
{
    if (s_rq_len >= T3_RQUEUE_MAX) {           /* drop-oldest */
        s_rq_head = (s_rq_head + 1) % T3_RQUEUE_MAX;
        s_rq_len--;
    }
    s_rq[(s_rq_head + s_rq_len) % T3_RQUEUE_MAX] = *rep;
    s_rq_len++;
}

void touch3_flush_reports(void)
{
    while (s_rq_len > 0) {
        if (!report_send(&s_rq[s_rq_head], NULL)) return;   /* keep order */
        s_rq_head = (s_rq_head + 1) % T3_RQUEUE_MAX;
        s_rq_len--;
    }
}

/* ---- drawing + refresh ------------------------------------------------ */

/* A framebuffer to draw feedback into. Prefers overlay's working copy of the
 * real frame; falls back to a paper-filled scratch buffer.
 *
 * The fallback is the whole reason feedback works on a battery wake. overlay
 * only holds a full frame copy immediately after a paint, so a touch wake that
 * gets a 304 has none -- and this used to skip feedback entirely, which looked
 * exactly like "the controls don't animate". v3 does not need the server's
 * image: it leaves every control rect BLANK and t3_draw_primitive fills the
 * rect with paper before drawing chrome, so a scratch buffer produces
 * byte-identical pixels inside the rect. Only that rect is ever streamed to the
 * panel, so the rest of the buffer's contents are irrelevant (still filled with
 * paper so a stray read is harmless rather than noise).
 *
 * *real tells the caller whether this is the true frame, because a full-screen
 * refresh is only safe on the real one -- a GC16 from scratch would paint a
 * blank panel. */
static uint8_t *s_fb_scratch;

static uint8_t *feedback_fb(bool *real)
{
    bool full = false;
    uint8_t *work = overlay_work_fb(&full);
    if (work && full) { *real = true; return work; }

    *real = false;
    if (!s_fb_scratch) {
        s_fb_scratch = heap_caps_malloc(EPD_BUF_BYTES,
                                        TESSERAE_FB_CAPS);
        if (!s_fb_scratch) {
            ESP_LOGW(TAG, "no PSRAM for the feedback scratch buffer");
            return NULL;
        }
        memset(s_fb_scratch, 0xFF, EPD_BUF_BYTES);   /* 0xF nibbles = paper */
        ESP_LOGI(TAG, "feedback scratch buffer allocated (%u KB)",
                 (unsigned)(EPD_BUF_BYTES / 1024));
    }
    return s_fb_scratch;
}

/* Redraw one primitive ready for a partial refresh. Returns the buffer to
 * stream from, or NULL when none could be had. */
static uint8_t *redraw_prim(const t3_prim_t *p, bool *real)
{
    uint8_t *fb = feedback_fb(real);
    if (!fb) return NULL;
    t3_draw_primitive(fb, EPD_WIDTH, EPD_HEIGHT, panel_bpp(), &s_spec, p);
    return fb;
}

/* Refresh a rect, forcing a quality GC16 every T3_HYGIENE_N partials on that
 * primitive (firmware-spec §9).
 *
 * Feedback uses DU -- the same waveform the v1/v2 overlay tap-echo path has
 * used in production all along, which is the strongest evidence available that
 * it behaves on this panel. Two faster alternatives were tried on real glass
 * 2026-07-27 and both rejected:
 *
 *   A2         ~220 ms, but visibly filthy ghosting.
 *   GRAY (5)   ~435 ms, clean, and the only tier that renders the `mid` fill
 *              on a switch's ON track as actual grey rather than collapsing it
 *              to black. Still rejected: a pressed button's inversion did not
 *              clear on the glass.
 *
 * Both remain available via epd_refresh_t and are documented in the driver, so
 * revisiting this is a one-line change. Cost of DU: ~960-1170 ms per rect,
 * knowingly 2x the contract's ~500 ms target, and being 2-level it renders the
 * `mid` greys as solid black. Shrinking the rect does not help -- the cost is
 * the waveform, not the pixel count.
 *
 * OPEN: the inversion-not-clearing report reproduced on BOTH A2 and GRAY and is
 * NOT understood. It is not the framebuffer -- a host test proves invert then
 * t3_draw_primitive restores byte-for-byte. The button is the only primitive
 * that partial-refreshes one rect twice in close succession, which is the
 * standing suspect. Untested on DU. */
static void refresh_prim(t3_prim_t *p, t3_rect_t r, uint8_t *fb, bool real)
{
    epd_refresh_t mode = EPD_RF_DU;
    if (++p->partials >= T3_HYGIENE_N) {
        mode = EPD_RF_GC16;
        p->partials = 0;
        ESP_LOGD(TAG, "'%s': hygiene GC16 after %d partials", p->id,
                 T3_HYGIENE_N);
    }
    if (real) {
        /* Through overlay so its shared hygiene counter stays honest. */
        overlay_partial_refresh_mode(r.x, r.y, r.w, r.h, mode);
        return;
    }
    /* Scratch buffer: stream the rect straight to the panel. Deliberately NOT
     * via overlay -- it would refresh from its own (absent) copy -- and the
     * hygiene pass stays a GC16 of this RECT, never a full-screen repaint,
     * which from a scratch buffer would blank the panel. */
    if (epd_port_init() != ESP_OK) return;
    epd_init();
    epd_display_partial_mode(fb, r.x, r.y, r.w, r.h, mode);
}

/* Redraw + refresh in one step (the common feedback path). */
static bool repaint_prim(t3_prim_t *p)
{
    bool real = false;
    uint8_t *fb = redraw_prim(p, &real);
    if (!fb) return false;
    refresh_prim(p, t3_feedback_rect(&s_spec, p), fb, real);
    return true;
}

/* Invert a rect in the working framebuffer -- the press affordance for
 * buttons and stepper zones. Uses the shared 4bpp/1bpp invert already proven
 * by the v2 echo path. */
static bool invert_rect(t3_prim_t *p, t3_rect_t r)
{
    bool real = false;
    /* Draw the primitive FIRST, then invert it. Idempotent on the real
     * framebuffer (the rect is self-contained), and required on the scratch
     * one, which has no image behind it to invert. */
    uint8_t *fb = redraw_prim(p, &real);
    if (!fb) return false;
    overlay_invert_rect(fb, EPD_WIDTH, EPD_HEIGHT, panel_bpp(),
                        r.x, r.y, r.w, r.h);
    refresh_prim(p, r, fb, real);
    return true;
}

/* ---- reconcile -------------------------------------------------------- */

/* Apply server-confirmed state to a primitive, repainting only if it differs
 * from what is already drawn (contract §6). */
static void reconcile(t3_prim_t *p, const rest_interact_out_t *cf)
{
    /* Nothing confirmed => the local draw stays OPTIMISTIC. The server's
     * /interact reply carries only {outcome, primitive_id} today; confirmed
     * switch/slider state is to arrive on the SSE values channel instead. So an
     * empty reply must NOT clear p->optimistic -- doing that would claim the
     * server had agreed with a state it never sent, and the SSE correction is
     * the only thing that can settle it. */
    if (!cf->have_state && !cf->have_value) return;

    bool changed = false;
    if (cf->have_state && p->type == T3_SWITCH && p->state != cf->state) {
        p->state = cf->state;
        changed = true;
    }
    if (cf->have_value && (p->type == T3_SLIDER || p->type == T3_STEPPER)) {
        float v = t3_snap(p, cf->value);
        if (v != p->value) { p->value = v; changed = true; }
    }
    p->optimistic = false;             /* the server did confirm something */
    if (!changed) return;
    ESP_LOGI(TAG, "'%s': reconciled to server state", p->id);
    repaint_prim(p);
}

/* Report an interaction. Tier 1 reconciles inline against the reply so the
 * glass settles in one round trip; tier 0 needs no confirmation and tier 2 is
 * settled by the following frame, so neither reconciles here. */
static void report_interaction(t3_prim_t *p, t3_gesture_t g, bool has_value,
                               float value)
{
    t3_report_t rep;
    memset(&rep, 0, sizeof rep);
    snprintf(rep.primitive_id, sizeof rep.primitive_id, "%s", p->id);
    snprintf(rep.interaction, sizeof rep.interaction, "%s",
             t3_interaction_name(g));
    rep.has_value = has_value;
    rep.value = value;
    snprintf(rep.layout_digest, sizeof rep.layout_digest, "%s",
             s_spec.layout_digest);
    rep.event_id = app_next_event_id();

    touch3_flush_reports();                 /* keep order: drain first */
    if (s_rq_len > 0) { rq_push(&rep); return; }

    rest_interact_out_t cf;
    if (!report_send(&rep, &cf)) {
        rq_push(&rep);
        /* Network down: keep the optimistic pixels. The server is still the
         * source of truth and corrects via SSE or the next frame; the device
         * never self-reverts. */
        return;
    }
    /* Outcome is informational only -- the feedback is already on glass, so
     * there is nothing to branch on (server sync 2026-07-27). "deduped" means a
     * replayed event_id, which is exactly what the queue's retries rely on. */
    if (cf.outcome[0])
        ESP_LOGI(TAG, "'%s': outcome %s", p->id, cf.outcome);
    reconcile(p, &cf);          /* no-op until the server sends confirmed state */
}

/* ---- touch ------------------------------------------------------------ */

/* A slider tap sets the value at the tapped position. The spec pins drag for
 * sliders and leaves a bare tap undefined (§8); jumping to the tapped point is
 * the only behaviour consistent with the drag math, and matches what the
 * canvas preview implies. */
static void slider_set(t3_prim_t *p, int fx, int fy, bool report)
{
    float v = t3_slider_value_at(p, fx, fy);
    if (v == p->value && !report) return;
    p->value = v;
    p->optimistic = true;
    repaint_prim(p);
    if (report) report_interaction(p, T3_G_DRAG, true, v);
}

void touch3_stroke_sample(int fx, int fy, void *ctx)
{
    (void)ctx;
    if (!touch3_active()) return;
    int64_t now = esp_timer_get_time();

    /* First sample of a stroke (or the first after a long gap, which means the
     * previous stroke never reached try_touch): latch which primitive the
     * finger went down on. Only sliders track live. */
    if (!s_drag_armed || now - s_drag_seen_us > T3_DRAG_IDLE_US) {
        int idx = t3_hit(&s_spec, fx, fy);
        s_drag_idx = (idx >= 0 && s_spec.prims[idx].type == T3_SLIDER)
                   ? idx : -1;
        s_drag_armed = true;
        s_drag_last_us = 0;
    }
    s_drag_seen_us = now;
    if (s_drag_idx < 0) return;

    /* Rate-limit: one DU costs ~250-450 ms on the E1003, samples arrive every
     * ~8 ms. Refreshing per sample would queue refreshes forever behind the
     * finger. */
    if (now - s_drag_last_us < T3_DRAG_MIN_US) return;
    t3_prim_t *p = &s_spec.prims[s_drag_idx];
    float v = t3_slider_value_at(p, fx, fy);
    if (v == p->value) return;                /* nothing moved on the grid */
    s_drag_last_us = now;
    p->value = v;
    p->optimistic = true;
    repaint_prim(p);                          /* thumb + value_text */
}

bool touch3_try_touch(int x0, int y0, int x1, int y1, uint32_t ms,
                      bool *want_frame_poll)
{
    if (want_frame_poll) *want_frame_poll = false;
    s_drag_armed = false;                     /* the stroke is over */
    s_drag_idx = -1;

    if (!s_have_spec) return false;           /* v2/v1 dispatch handles it */
    /* An EMPTY spec is valid and expected: the touch_v3 experiment is off, or
     * this page simply has no controls. v3 then holds nothing, so decline the
     * stroke rather than swallowing it -- claiming it would silently disable the
     * v1/v2 touch that works today on every device with the flag off, which
     * would be a straight regression until the contract's Phase 3 removes
     * them. "Latch touch off" applies to v3's own surface. */
    if (s_spec.n_prims == 0) return false;
    if (!t3_spec_matches(&s_spec, s_glass_layout)) {
        /* The held spec does not describe what is on the glass, so hit-testing
         * against it would dispatch the wrong control (firmware-spec §13).
         * Decline rather than claim the stroke: v2 checks its own digest anchor
         * and will decline too, leaving the v1 coordinate path -- which the
         * server hit-tests against the frame actually served. That is the safe
         * degradation, and it cannot wedge if a paint is ever skipped after a
         * spec fetch. */
        ESP_LOGI(TAG, "layout mismatch (spec %s, glass %s); deferring",
                 s_spec.layout_digest, s_glass_layout);
        return false;
    }

    /* Debounce: a second down within 120 ms of an up on the same primitive is
     * a contact bounce, not a second press (firmware-spec §8). */
    static int64_t s_last_up_us;
    static int     s_last_idx = -1;
    int idx = t3_hit(&s_spec, x0, y0);
    int64_t now = esp_timer_get_time();
    if (idx >= 0 && idx == s_last_idx &&
        now - s_last_up_us < T3_DEBOUNCE_MS * 1000) {
        ESP_LOGD(TAG, "debounced repeat on '%s'", s_spec.prims[idx].id);
        return true;
    }
    s_last_up_us = now;
    s_last_idx = idx;

    if (idx < 0) {
        /* A MISS falls through instead of being swallowed. The contract has v3
         * owning the whole surface, but that assumes Phase 3 has removed v1/v2
         * -- and it has not. v3 has no swipe concept at all (t3_classify only
         * yields tap or drag), whereas protocol v2 uses swipes for PAGE NAV via
         * its bundle links graph. Claiming a stroke that landed on no control
         * therefore killed swipe navigation outright.
         *
         * Declining costs nothing: v3 did not act, so nothing is consumed, and
         * v2 (then v1) get the stroke exactly as before. Revisit in Phase 3,
         * when v3 genuinely is the only layer. */
        ESP_LOGD(TAG, "no primitive at (%d,%d); deferring to v2/v1", x0, y0);
        return false;
    }
    t3_prim_t *p = &s_spec.prims[idx];
    t3_gesture_t g = t3_classify(x0, y0, x1, y1, ms);

    switch (p->type) {
    case T3_BUTTON: {
        /* Momentary: invert as the press affordance, report, then restore.
         * Tier 2 shows the inverted state as its pending affordance until the
         * next frame lands, rather than a fake optimistic result (§9). */
        int64_t t_press = esp_timer_get_time();
        invert_rect(p, p->rect);
        report_interaction(p, T3_G_TAP, false, 0);
        if (p->tier != 2) {
            /* Keep the press visible for T3_PRESS_MIN_MS before restoring --
             * /interact can return far faster than the eye (or, on the bench,
             * than the panel) copes with. The feedback is already on glass, so
             * waiting here costs the user nothing. */
            int32_t held = (int32_t)((esp_timer_get_time() - t_press) / 1000);
            if (held < T3_PRESS_MIN_MS)
                vTaskDelay(pdMS_TO_TICKS(T3_PRESS_MIN_MS - held));
            repaint_prim(p);                        /* restore the chrome */
        }
        break;
    }

    case T3_SWITCH:
        /* Optimistic flip, then reconcile on the reply. */
        p->state = !p->state;
        p->optimistic = true;
        repaint_prim(p);
        report_interaction(p, T3_G_TAP, false, 0);
        break;

    case T3_SLIDER:
        /* A drag has already been tracking live through touch3_stroke_sample;
         * settle on the lift position and report once. */
        slider_set(p, x1, y1, true);
        break;

    case T3_STEPPER: {
        int zone = t3_stepper_zone(p, x0, y0);
        if (zone == 0) break;                       /* the value third is inert */
        t3_rect_t zr = t3_stepper_zone_rect(p, zone);
        invert_rect(p, zr);                         /* zone press affordance */
        float v = t3_snap(p, p->value + (zone > 0 ? p->vstep : -p->vstep));
        bool moved = v != p->value;
        p->value = v;
        p->optimistic = true;
        repaint_prim(p);                            /* clears the invert too */
        if (moved) report_interaction(p, T3_G_DRAG, true, v);
        break;
    }
    }

    /* Tier 2 and the nav-like actions settle on a new frame; tier 0/1 do not. */
    bool nav_like = p->atype == T3_ACT_NAV || p->atype == T3_ACT_REFRESH ||
                    p->atype == T3_ACT_FETCH;
    if (want_frame_poll) *want_frame_poll = (p->tier == 2) || nav_like;

    ESP_LOGI(TAG, "%s '%s' (%s) tier %d", t3_interaction_name(g), p->id,
             t3_ptype_name(p->type), p->tier);
    return true;
}

/* ---- values stream: live state reconcile ------------------------------
 * Envelope (server sync 2026-07-27):
 *   {"seq": <ms>, "values": {"ha:light.desk": "on", ...}}
 * Keyed by VALUE_KEY, not by primitive id -- the server has no idea which
 * primitives exist, so the key -> primitive mapping is ours. Several primitives
 * may share a key (or bind different dotted paths of one entity), so every
 * match is applied, not just the first. */

/* Apply one value string to one primitive. True when the drawn state moved. */
static bool apply_value(t3_prim_t *p, const char *val)
{
    if (p->type == T3_SWITCH) {
        bool on = strcasecmp(val, "on") == 0 || strcasecmp(val, "true") == 0 ||
                  strcmp(val, "1") == 0 || strcasecmp(val, "open") == 0 ||
                  strcasecmp(val, "home") == 0;
        if (on == p->state) return false;
        p->state = on;
        return true;
    }
    if (p->type == T3_SLIDER || p->type == T3_STEPPER) {
        char *end = NULL;
        float raw = strtof(val, &end);
        if (end == val) return false;             /* "unavailable" etc. */
        float nv = t3_snap(p, raw);
        if (nv == p->value) return false;
        p->value = nv;
        return true;
    }
    return false;                                 /* buttons hold no state */
}

void touch3_ingest_values(const char *json, size_t len)
{
    if (!json || !len || !touch3_active()) return;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;

    /* Newest seq wins, same stream semantics as the overlay/v2 values docs: a
     * late-arriving older envelope must not undo a newer one. */
    const cJSON *sq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    if (cJSON_IsNumber(sq)) {
        int64_t seq = (int64_t)sq->valuedouble;
        if (seq < s_values_seq) { cJSON_Delete(root); return; }
        s_values_seq = seq;
    }

    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(root, "values");
    if (!cJSON_IsObject(vals)) vals = root;   /* tolerate a bare map */

    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, vals) {
        if (!it->string) continue;
        /* Values are pre-formatted strings; accept a raw number too. */
        char buf[32];
        const char *val = NULL;
        if (cJSON_IsString(it))      val = it->valuestring;
        else if (cJSON_IsBool(it))   val = cJSON_IsTrue(it) ? "on" : "off";
        else if (cJSON_IsNumber(it)) {
            snprintf(buf, sizeof buf, "%g", it->valuedouble);
            val = buf;
        }
        if (!val) continue;

        for (int i = 0; i < s_spec.n_prims; i++) {
            t3_prim_t *p = &s_spec.prims[i];
            if (!p->value_key[0] || strcmp(p->value_key, it->string) != 0)
                continue;
            if (!apply_value(p, val)) { p->optimistic = false; continue; }
            /* The server has spoken: this is no longer an optimistic guess. */
            p->optimistic = false;
            ESP_LOGI(TAG, "'%s' <- %s = %s", p->id, it->string, val);
            repaint_prim(p);
        }
    }
    cJSON_Delete(root);
}

#endif /* BOARD_TOUCH3 */
