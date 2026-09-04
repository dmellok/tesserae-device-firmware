/*
 * proto2.h: pure logic for Tesserae protocol v2 (device-owned touch).
 *
 * v2 moves hit testing and immediate visual feedback onto the device: the
 * server serves an INTERACTION MANIFEST per frame (regions + text regions +
 * font atlases), the firmware classifies gestures, applies tiered local
 * feedback, and reports the action; the server stays the source of truth
 * and corrects via patches/values/frames (contract: "Firmware implementation
 * prompt: Tesserae protocol v2", 2026-07-26).
 *
 * This file is PURE (cJSON + libc only) and host-tested by
 * test/test_proto2.c, mirroring overlay.h/deck.h. Device orchestration
 * (fetch, SD bundles, SSE, tier engine, report queue) lives in proto2_run.c.
 *
 * All coordinates are WIRE FRAMEBUFFER pixels (origin top-left, before any
 * panel-driver mirroring); the firmware never transforms server rects.
 * Wire pixel format everywhere (frames, tiles, patches, atlases): 4bpp
 * grayscale linear (0x0 black .. 0xF white), 2 px/byte, HIGH nibble = left
 * pixel, row-major, no padding; every rect w and x are even.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define P2_PROTO          2
#define P2_MAX_REGIONS    64
#define P2_MAX_TEXT       8
#define P2_MAX_ATLASES    2
#define P2_MAX_GLYPHS     64
#define P2_MAX_CYCLE      4
#define P2_ID_CAP         40   /* "el:tile_desk:tap", "st:tile_desk", "page:<16hex>" */
#define P2_NAME_CAP       16   /* tile cycle state names */
#define P2_KEY_CAP        64   /* "ha:light.desk:attributes.brightness_pct" */
#define P2_URL_CAP        160
#define P2_VALUE_CAP      48   /* values are pre-formatted, <= 47 chars */
#define P2_DIGEST_HEX     16

/* Gesture classification thresholds (wire pixels / ms). */
#define P2_TAP_MAX_MS     400
#define P2_TAP_MAX_PX     20
#define P2_SWIPE_MIN_PX   40

typedef enum {
    P2_ACT_NAV = 0, P2_ACT_HA, P2_ACT_WEBHOOK, P2_ACT_REFRESH,
    P2_ACT_FETCH_LATEST,
    P2_ACT_UNKNOWN,   /* forward-compat: downgraded to invert + tier 2 */
} p2_action_type_t;

typedef enum {
    P2_FB_INVERT = 0, P2_FB_TILES, P2_FB_SLIDER, P2_FB_NONE,
    /* unknown feedback.mode parses as P2_FB_INVERT (downgrade rule) */
} p2_feedback_t;

/* Swipe-direction bits (gestures.swipe.{left,right,up,down}). */
#define P2_SW_LEFT   0x1
#define P2_SW_RIGHT  0x2
#define P2_SW_UP     0x4
#define P2_SW_DOWN   0x8

typedef struct {
    char id[P2_ID_CAP];
    int  x, y, w, h;
    bool g_tap;
    uint8_t g_swipe;              /* P2_SW_* mask */
    bool g_slide;
    char slide_axis;              /* 'x' or 'y' (valid when g_slide) */
    int  tier;                    /* 0 | 1 | 2 */
    p2_action_type_t type;
    char target[P2_ID_CAP];       /* nav: bundle state id ("page:...") */
    p2_feedback_t fb;
    char fb_set[P2_ID_CAP];       /* tiles: state-set name ("st:tile_desk") */
    int  n_cycle;
    char cycle[P2_MAX_CYCLE][P2_NAME_CAP];
    char fb_track;                /* slider: 'v' | 'h' */
    char fb_value_text[P2_ID_CAP];/* slider: text region id to update live */
} p2_region_t;

typedef struct {
    char     ch;                  /* single-byte character key */
    uint16_t x, w;                /* horizontal slice of the strip */
} p2_glyph_t;

typedef struct {
    char digest[P2_DIGEST_HEX + 1];
    char url[P2_URL_CAP];
    int  height;
    int  n_glyphs;
    p2_glyph_t glyphs[P2_MAX_GLYPHS];
    int  strip_w;                 /* derived: max(x + w); always even */
    int  mean_w;                  /* derived: blank width for unknown chars */
    /* Strip bits, attached by the loader after fetch + digest verify; not
     * part of the parse. 4bpp, 2 px/byte, high nibble left. */
    const uint8_t *bits;
} p2_atlas_t;

typedef struct {
    char id[P2_ID_CAP];
    int  x, y, w, h;
    int  align;                   /* 0 left, 1 center, 2 right */
    int  atlas_idx;               /* into manifest atlases[] (digest-deduped) */
    char key[P2_KEY_CAP];         /* values key, or "local:clock:HH.MM" */
    int  max_chars;
    char value[P2_VALUE_CAP];     /* last rendered string, "" = none yet */
} p2_text_t;

typedef struct {
    char frame_digest[P2_DIGEST_HEX + 1];
    char manifest_digest[P2_DIGEST_HEX + 1];
    int  n_regions;
    p2_region_t regions[P2_MAX_REGIONS];
    int  n_text;
    p2_text_t   text[P2_MAX_TEXT];
    int  n_atlases;
    p2_atlas_t  atlases[P2_MAX_ATLASES];
} p2_manifest_t;

/* Parse a GET /frame/manifest 200 body. Strict on proto == 2 and both
 * digests; individually DROPS malformed regions/text (and text whose atlas
 * is unusable) rather than failing the manifest. Unknown feedback.mode or
 * action.type DOWNGRADES that region to invert + tier 2 (never a reject).
 * Regions keep document order: FIRST HIT WINS. Atlases are deduped by
 * digest into <= P2_MAX_ATLASES slots. Rects are clamped to panel bounds;
 * fully off-panel rects drop their region. */
bool p2_manifest_parse(const char *json, size_t len,
                       int panel_w, int panel_h, p2_manifest_t *out);

/* True iff the manifest anchors to frame digest d. */
bool p2_manifest_matches(const p2_manifest_t *m, const char *d);

/* ---- gesture classification + hit test ---- */

typedef enum {
    P2_G_NONE = 0,
    P2_G_TAP,
    P2_G_SWIPE_LEFT, P2_G_SWIPE_RIGHT, P2_G_SWIPE_UP, P2_G_SWIPE_DOWN,
    P2_G_SLIDE,       /* produced by hit-test only (slide regions absorb) */
} p2_gesture_t;

/* Classify a raw stroke. Contract thresholds: tap = lift < 400 ms AND
 * displacement < 20 px; swipe = displacement >= 40 px, dominant axis,
 * direction = dominant sign. Deviation, flagged in the v2 report: a press
 * held past 400 ms with < 20 px displacement still classifies as TAP (the
 * contract leaves long-press undefined; bench strokes routinely run
 * 400-900 ms and dropping them would eat half of real-world taps). */
p2_gesture_t p2_classify(int x0, int y0, int x1, int y1, uint32_t ms);

/* Hit-test a classified stroke against the manifest. Regions are checked
 * in document order (first hit wins) against the DOWN point; a region
 * declaring slide absorbs any gesture that lands in it (returns P2_G_SLIDE
 * + value 0-100 from the LIFT position along the axis; vertical fills
 * upward, horizontal fills rightward). A swipe that misses on the down
 * point retries against the LIFT point. Returns the region index or -1;
 * *out_gesture is the effective gesture, *out_value the slide value
 * (untouched for non-slides). */
int p2_hit(const p2_manifest_t *m, int x0, int y0, int x1, int y1,
           uint32_t ms, p2_gesture_t *out_gesture, int *out_value);

/* ---- text rendering (4bpp only; wire format) ---- */

/* Glyph lookup / string width in atlas a (unknown chars count mean_w). */
const p2_glyph_t *p2_glyph(const p2_atlas_t *a, char ch);
int p2_text_width(const p2_atlas_t *a, const char *text);

/* Render `str` into t's rect on a 4bpp framebuffer: clip to max_chars,
 * clear the rect to white, blit glyph slices left-to-right honouring
 * align, vertical-center the atlas band. Handles the odd/even column
 * phase (nibble-shifting blit). No-op when the atlas has no bits. */
void p2_draw_text(uint8_t *fb, int fb_w, int fb_h, int bpp,
                  const p2_text_t *t, const p2_atlas_t *a, const char *str);

/* ---- gesture name strings (report bodies) ---- */
const char *p2_gesture_name(p2_gesture_t g);   /* "tap"/"swipe_left"/... */

/* ---- state bundles (§8: tier-0 nav + toggle tiles) ---- */

#define P2_MAX_BSTATES 24
#define P2_MAX_LINKS   16

typedef enum { P2_BK_FRAME = 0, P2_BK_TILE } p2_bkind_t;

typedef struct {
    p2_bkind_t kind;
    char     state_id[P2_ID_CAP];      /* "page:<16hex>" / "st:set/name" */
    char     digest[P2_DIGEST_HEX + 1];/* frame_digest or tile_digest */
    char     man_digest[P2_DIGEST_HEX + 1]; /* frames: manifest_digest ("") */
    int      x, y, w, h;               /* tiles: target rect */
    uint32_t bytes;
    int32_t  ttl_s;                    /* frames: 0 = no ttl */
    char     url[P2_URL_CAP];
} p2_bstate_t;

typedef struct {
    char from[P2_ID_CAP];              /* source page state id */
    char key[P2_ID_CAP];               /* "swipe_left" or a region id */
    char to[P2_ID_CAP];                /* target page state id */
} p2_blink_t;

typedef struct {
    char bundle_digest[P2_DIGEST_HEX + 1];
    int  n_states;
    p2_bstate_t states[P2_MAX_BSTATES];
    int  n_links;
    p2_blink_t links[P2_MAX_LINKS];
} p2_bundle_t;

/* Parse a GET /bundle 200 body. Strict on bundle_digest; individually
 * drops malformed states/links; frame states validate bytes against the
 * panel size the caller passes (frame_bytes, 0 = skip the check). */
bool p2_bundle_parse(const char *json, size_t len, uint32_t frame_bytes,
                     p2_bundle_t *out);

/* State lookup by id; NULL when absent. */
const p2_bstate_t *p2_bundle_state(const p2_bundle_t *b, const char *state_id);

/* Link lookup: the target state id for (from_page, key), else NULL. key is
 * a region id ("el:nav_next:tap") or a bare gesture name ("swipe_left"). */
const char *p2_bundle_link(const p2_bundle_t *b, const char *from,
                           const char *key);
