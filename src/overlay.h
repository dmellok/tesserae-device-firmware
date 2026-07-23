/*
 * overlay.h: pure logic for the local overlay render mode (hybrid render).
 *
 * The server's full frame stays the base layer and source of truth; the
 * firmware overlays a small set of SERVER-DECLARED primitives onto its copy
 * of the framebuffer and partial-refreshes only those rects for sub-second
 * feedback on touch boards. Nothing here is invented by the firmware: every
 * rect, glyph and string comes from the overlay spec / values documents
 * (contract: GET /frame/overlay/<digest>, GET /frame/data?digest=...).
 *
 * This file is PURE (cJSON + libc only) and host-tested by
 * test/test_overlay.c, mirroring deck.h. Device-side orchestration (fetch,
 * SD cache, partial-refresh calls, wake handling) lives in overlay_run.c.
 *
 * All coordinates are in the SAME pixel space as the wire framebuffer
 * (firmware-native, row-major, post-everything); the firmware never rotates,
 * scales or normalises them -- only clamps to the panel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OVERLAY_SCHEMA        1
/* Grid-editor dashboards emit whole-cell targets, so dense boards need far
 * more than the original 8; 32 static targets cost <1 KB of parser state.
 * The firmware advertises this cap ("overlay": {"schema": 1, "max_targets":
 * 32}) so the server trims per device instead of assuming. Overflow rule:
 * extras beyond the cap are dropped individually in document order (first N
 * win) and the spec stays valid -- never a wholesale reject. */
#define OVERLAY_MAX_TARGETS   32
#define OVERLAY_MAX_SLOTS     8
#define OVERLAY_MAX_ATLASES   2
#define OVERLAY_MAX_GLYPHS    32
#define OVERLAY_ID_CAP        8
#define OVERLAY_KEY_CAP       40
#define OVERLAY_VALUE_CAP     48
#define OVERLAY_URL_CAP       160
#define OVERLAY_DIGEST_HEX    16

/* Hygiene: after this many partial refreshes, do a full-quality repaint. */
#define OVERLAY_HYGIENE_LIMIT 8

typedef enum { OVERLAY_ECHO_INVERT = 1 } overlay_echo_t;
typedef enum { OVERLAY_ALIGN_LEFT = 0, OVERLAY_ALIGN_CENTER, OVERLAY_ALIGN_RIGHT } overlay_align_t;

typedef struct {
    char id[OVERLAY_ID_CAP];
    int  x, y, w, h;
    overlay_echo_t echo;
} overlay_target_t;

typedef struct {
    char id[OVERLAY_ID_CAP];
    int  x, y, w, h;
    char key[OVERLAY_KEY_CAP];
    overlay_align_t align;
    char atlas_id[OVERLAY_ID_CAP];
    char value[OVERLAY_VALUE_CAP];   /* latest display string, "" = none yet */
} overlay_slot_t;

typedef struct {
    char     ch;                     /* single-byte character key */
    uint16_t x, w;                   /* slice of the horizontal strip */
} overlay_glyph_t;

typedef struct {
    char     id[OVERLAY_ID_CAP];
    char     digest[OVERLAY_DIGEST_HEX + 1];
    int      height;
    char     url[OVERLAY_URL_CAP];
    uint8_t  bpp;                    /* 4 = "4bpp-gray", 1 = "1bpp" */
    int      n_glyphs;
    overlay_glyph_t glyphs[OVERLAY_MAX_GLYPHS];
    int      strip_w;                /* derived: max(x + w) over glyphs */
    int      mean_w;                 /* derived: fallback width for missing chars */
    /* Strip bits, attached by the loader after fetching/verifying; not part
     * of the parse. Row-major, strip_w x height at bpp. */
    const uint8_t *bits;
} overlay_atlas_t;

typedef struct {
    char frame_digest[OVERLAY_DIGEST_HEX + 1];
    int  n_targets, n_slots, n_atlases;
    overlay_target_t targets[OVERLAY_MAX_TARGETS];
    overlay_slot_t   slots[OVERLAY_MAX_SLOTS];
    overlay_atlas_t  atlases[OVERLAY_MAX_ATLASES];
} overlay_spec_t;

/* Parse a GET /frame/overlay/<digest> 200 body. Strict on schema + digest
 * shape; individually DROPS malformed targets/slots (and slots referencing
 * an unknown atlas id) rather than failing the spec -- rect-only specs
 * (targets without slots/atlases) are valid. Rects are clamped to
 * panel_w x panel_h; a rect fully outside the panel is dropped. Returns
 * false (and zeroes *out) only when the document as a whole is unusable. */
bool overlay_spec_parse(const char *json, size_t len,
                        int panel_w, int panel_h, overlay_spec_t *out);

/* True iff the spec belongs to frame digest `d` (staleness rule). */
bool overlay_spec_matches(const overlay_spec_t *s, const char *d);

/* Tap hit-test: the first target containing (x, y), else NULL. */
const overlay_target_t *overlay_hit_target(const overlay_spec_t *s, int x, int y);

/* Atlas lookup by id; NULL when absent. */
overlay_atlas_t *overlay_find_atlas(overlay_spec_t *s, const char *id);

/* Glyph for `ch`, or NULL (caller renders a blank of atlas->mean_w). */
const overlay_glyph_t *overlay_glyph(const overlay_atlas_t *a, char ch);

/* Pixel width of `text` in `a` (missing chars count mean_w). */
int overlay_text_width(const overlay_atlas_t *a, const char *text);

/* ---- values document ---- */

/* Apply a values document ({"seq": n, "values": {key: str}}) to the spec's
 * slots. `last_seq` is the newest seq applied so far (in/out): an older or
 * equal seq is ignored entirely (returns 0). Returns a bitmask of slot
 * indices whose value CHANGED (bit i = slots[i] needs a redraw). */
uint32_t overlay_values_apply(overlay_spec_t *s, const char *json, size_t len,
                              int32_t *last_seq);

/* ---- framebuffer operations (pure byte math; fb is panel-native) ---- */

/* Invert a rect in a packed framebuffer (bpp 4 or 1; rect pre-clamped by
 * parse). Whole-byte columns invert cheaply; edge pixels are handled. */
void overlay_invert_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                         int x, int y, int w, int h);

/* Restore a slot's rect from the pristine base frame, then blit `slot->value`
 * using its atlas (top-aligned, clipped to the slot rect, align honoured;
 * chars missing from the atlas render as mean-width blanks). No-op when the
 * atlas has no bits attached. */
void overlay_draw_slot(uint8_t *fb, const uint8_t *base, int fb_w, int fb_h,
                       int bpp, const overlay_slot_t *slot,
                       const overlay_atlas_t *atlas);

/* ---- hygiene counter ---- */

typedef struct { int partials; } overlay_hygiene_t;

static inline void overlay_hygiene_reset(overlay_hygiene_t *h) { h->partials = 0; }
static inline bool overlay_hygiene_tick(overlay_hygiene_t *h)
{
    /* Count one partial refresh; true = time for a full-quality repaint
     * (caller repaints and resets). */
    return ++h->partials >= OVERLAY_HYGIENE_LIMIT;
}
