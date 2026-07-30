/*
 * touch3.h: pure logic for Tesserae touch v3 (device-owned touch primitives).
 *
 * v3 replaces the v2 interaction manifest with a much smaller contract: the
 * server ships a dashboard image with the control rects left BLANK plus a
 * declarative touch spec; the firmware DRAWS the four primitives (button,
 * switch, slider, stepper) into those rects, owns hit-testing, gives instant
 * local partial-refresh feedback, and reports a SEMANTIC event (which
 * control, what interaction, what value). Action payloads never reach the
 * device -- it learns only a tier + type.
 *
 * Contract: notes/design-handoffs/touch-v3/ in the Tesserae server repo
 * (contract.md, firmware-spec.md, primitives.json v3.0.0,
 * frame-spec.schema.json, atlas.schema.json), 2026-07-27. Where
 * firmware-spec.md and firmware-build-prompt.md disagree, the spec wins.
 *
 * This file is PURE (cJSON + libc only) and host-tested by test/test_touch3.c,
 * mirroring proto2.h / overlay.h / deck.h. Device orchestration (fetch, atlas
 * loading, partial refresh, /interact, SSE) lives in touch3_run.c.
 *
 * TWO COORDINATE/INK CONVENTIONS MEET HERE -- read before editing:
 *
 * 1. Coordinates. Spec rects arrive in DEVICE FRAMEBUFFER space: the server
 *    already applied the orientation/scale/underscan transform (firmware-spec
 *    §2). The firmware draws directly at each rect and never re-rotates.
 *
 * 2. Ink levels. primitives.json speaks INK scale (0 = paper .. 15 = full
 *    ink), and so do v3 atlas strips (atlas.schema.json: "0 = paper .. 15 =
 *    full ink"). The device framebuffer is the opposite: 4bpp grayscale with
 *    0x0 = BLACK .. 0xF = WHITE (see the board headers' EPD_COL_ values and
 *    proto2.h). Every
 *    write therefore goes through T3_FB() / t3_fb_level(), which invert. Note
 *    this is a REAL difference from v2 atlases, which were already in panel
 *    (white-scale) order and blitted straight through.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define T3_TOUCH_V        3

/* A real dashboard runs well past the contract's illustrative examples: the
 * live server sent 46 primitives for one page, which silently truncated at the
 * old cap of 32. Matches proto2's P2_MAX_REGIONS for the same reason. */
#define T3_MAX_PRIMS      64
#define T3_MAX_ATLASES    2    /* label (20/400) + value (28/700) */

#define T3_ID_CAP         33   /* frame-spec: ^[a-z0-9_]{1,32}$ */
/* Real keys reach past 48: "ha:climate.<long_entity>:attributes.temperature".
 * A TRUNCATED key is worse than a dropped one -- it parses fine and then never
 * matches the values stream, so the control silently stops reconciling. 64
 * matches proto2's P2_KEY_CAP. */
#define T3_KEY_CAP        64
#define T3_URL_CAP        160
#define T3_DIGEST_CAP     33   /* content hash, hex; 16-hex is the house form */
#define T3_LABEL_CAP      24   /* static label text */
#define T3_SUFFIX_CAP     8    /* "%" or "°" (2 UTF-8 bytes) + NUL */
#define T3_ICON_NAME_CAP  24   /* Phosphor icon name, e.g. "film-slate" */
#define T3_VALTEXT_CAP    16   /* formatted value + suffix */

/* ---- ink model (primitives.json "palette" / "duotone") ------------------
 * Ink scale, 0 = paper .. 15 = full ink. Convert with T3_FB() before writing
 * a framebuffer. */
#define T3_INK_PAPER      0
#define T3_INK_SOFT       3
#define T3_INK_MID        8
#define T3_INK_INK        15
#define T3_DUO_PRIMARY    15
#define T3_DUO_SECONDARY  3

/* Ink level -> 4bpp panel nibble (0x0 black .. 0xF white). */
#define T3_FB(ink)        ((uint8_t)(15 - (ink)))

/* ---- geometry tokens (primitives.json "tokens" + per-primitive blocks) ---
 * Absolute device pixels, kept crisp on e-ink. These MUST track
 * primitives.json v3.0.0: drift breaks WYSIWYG against the server's canvas
 * preview, which is the primary fidelity risk in the contract (contract §5). */
#define T3_STROKE         2
#define T3_STROKE_BOLD    3
#define T3_CORNER_RADIUS  12
#define T3_PAD            12
#define T3_GAP            8
#define T3_MIN_TOUCH_PX   44

#define T3_SWITCH_GAP     10   /* label -> track */
#define T3_SWITCH_TRACK_H 44   /* min(rect.h, 44) */
#define T3_SWITCH_INSET   4    /* thumb inset inside the track */

#define T3_SLIDER_THICK   8    /* track thickness */
#define T3_SLIDER_RADIUS  4
#define T3_SLIDER_INSET   20   /* from each end of the rect along the axis */
#define T3_SLIDER_THUMB_D 36

#define T3_STEP_MARK_FRAC_NUM 28   /* marks.length_frac 0.28, integer math */
#define T3_STEP_MARK_FRAC_DEN 100

#define T3_ICON_PX        40   /* button content icon default */

/* Slider value_text placement. OPEN ITEM in the contract (contract §9:
 * "Decide slider value_text placement per axis (leading vs above-thumb)"):
 * primitives.json says "leading", firmware-spec §7 says "centered above
 * thumb, clamped inside R". We follow the more specific firmware-spec, and
 * keep it a one-line flip for when the server side pins it. */
#ifndef T3_SLIDER_VTEXT_ABOVE_THUMB
#define T3_SLIDER_VTEXT_ABOVE_THUMB 1
#endif

/* ---- gesture thresholds (firmware-spec §8) ---- */
#define T3_TAP_R          30   /* px: displacement below this is a tap */
#define T3_TAP_MAX_MS     500
#define T3_DEBOUNCE_MS    120

/* ---- refresh hygiene (firmware-spec §9) ---- */
#define T3_HYGIENE_N      8    /* DU partials per rect before a forced GC16 */

/* Minimum time a button's press inversion stays on the glass before it is
 * restored. Two reasons, and the second is the load-bearing one:
 *
 *   1. UX -- the whole point is that the press is PERCEPTIBLE. Production
 *      restores as soon as /interact returns, and against a LAN server that
 *      can be ~30 ms, which no one would see.
 *   2. The same rect then gets a second partial refresh right behind the
 *      first. On the E1003 bench, restoring after ~120 ms left the rect stuck
 *      inverted; at ~400 ms it cleared. Two variables changed between those
 *      runs (waveform and delay), so this is not a proven root cause -- it is
 *      a cheap guard that makes the production path no tighter than the timing
 *      that was observed working. */
#define T3_PRESS_MIN_MS   400

typedef enum { T3_BUTTON = 0, T3_SWITCH, T3_SLIDER, T3_STEPPER } t3_ptype_t;

typedef enum {
    T3_ACT_NAV = 0, T3_ACT_HA, T3_ACT_WEBHOOK, T3_ACT_REFRESH, T3_ACT_FETCH,
    T3_ACT_UNKNOWN,        /* forward-compat: treated as tier 2 */
} t3_atype_t;

typedef enum { T3_ALIGN_LEFT = 0, T3_ALIGN_CENTER, T3_ALIGN_RIGHT } t3_align_t;

typedef struct { int16_t x, y, w, h; } t3_rect_t;

/* ---- atlas (atlas.schema.json) --------------------------------------------
 * 4bpp gray strip, high nibble = left pixel, INK scale. Uniform glyph height
 * (strip_h), so a blit is a horizontal copy. Glyph slots live in a direct table
 * indexed by character (fast, no search).
 *
 * TEXT ONLY. The atlas carried no icons in the finalized contract (server sync
 * 2026-07-27): button icons render from the bundled Phosphor font instead, so
 * the earlier "@name" atlas-icon fallback is gone. A stray "@..." key parses as
 * an unrecognised glyph key and is skipped.
 *
 * Each packed cell is ADVANCE-wide, i.e. adv == w. The layout loop still blits
 * {x,w} and advances by adv, so it stays correct either way. */
#define T3_GLYPH_FIRST    32   /* ' ' */
#define T3_GLYPH_LAST     126  /* '~' */
#define T3_GLYPH_DEGREE   95   /* slot for U+00B0, the one non-ASCII char */
#define T3_NGLYPH         96

typedef struct { uint16_t x, w, adv; } t3_glyph_t;

typedef struct {
    char id[8];                          /* "l20", "v28" */
    char digest[T3_DIGEST_CAP];
    char url[T3_URL_CAP];
    int  px, weight;
    int  strip_w, strip_h;               /* strip_w derived when absent */
    int  ascent, descent, space_adv;
    bool have[T3_NGLYPH];
    t3_glyph_t glyphs[T3_NGLYPH];
    int  mean_w;                         /* derived: blank for unknown chars */
    int  digit_w;                        /* derived: widest '0'-'9' advance */
    /* Strip bytes, attached by the loader after fetch + verify (not parsed).
     * INK scale -- t3_draw_text inverts on the way out. */
    const uint8_t *bits;
} t3_atlas_t;

/* ---- text / icon refs (frame-spec $defs) ---- */

typedef struct {
    bool       present;
    int        atlas_idx;                /* into spec->atlases, -1 = unresolved */
    t3_align_t align;
    char       text[T3_LABEL_CAP];       /* static label; "" = value-bound */
    char       suffix[T3_SUFFIX_CAP];
    int        max_chars;                /* 0 = unbounded */
} t3_text_ref_t;

typedef struct {
    bool present;
    char name[T3_ICON_NAME_CAP];
    int  px;
    bool duotone;
} t3_icon_ref_t;

/* ---- primitive ---- */

typedef struct {
    char        id[T3_ID_CAP];
    t3_ptype_t  type;
    t3_rect_t   rect;
    int         tier;                    /* 0 | 1 | 2 */
    t3_atype_t  atype;

    t3_text_ref_t label;
    t3_text_ref_t value_text;
    t3_icon_ref_t icon;

    char        value_key[T3_KEY_CAP];

    /* numeric primitives (slider, stepper) */
    float       vmin, vmax, vstep, value;
    char        axis;                    /* 'x' | 'y' (slider) */

    /* switch */
    bool        state;                   /* true = on */

    /* runtime bookkeeping (not parsed) */
    bool        optimistic;              /* local state awaiting confirmation */
    uint8_t     partials;                /* DU count since the last GC16 */
} t3_prim_t;

typedef struct {
    char       layout_digest[T3_DIGEST_CAP];
    int        n_atlases;
    t3_atlas_t atlases[T3_MAX_ATLASES];
    int        n_prims;
    t3_prim_t  prims[T3_MAX_PRIMS];
} t3_spec_t;

/* ---- spec parse ---------------------------------------------------------- */

/* Parse a GET /frame/spec 200 body (frame-spec.schema.json). Strict on
 * layout_digest; individually DROPS malformed primitives rather than failing
 * the spec, and SKIPS primitives whose type is unknown (a newer server) --
 * both per firmware-spec §13. Rects are clamped to the panel; a rect fully
 * off-panel or with no area drops its primitive. Primitives keep document
 * order: FIRST HIT WINS. Atlases dedupe by digest into <= T3_MAX_ATLASES. */
bool t3_spec_parse(const char *json, size_t len,
                   int panel_w, int panel_h, t3_spec_t *out);

/* True iff the spec anchors to layout digest d. */
bool t3_spec_matches(const t3_spec_t *s, const char *d);

/* Primitive lookup by id; NULL when absent (a touch that raced a frame
 * change, firmware-spec §13). */
t3_prim_t *t3_prim_by_id(t3_spec_t *s, const char *id);

/* ---- hit test + gestures (firmware-spec §8) ------------------------------ */

typedef enum { T3_G_NONE = 0, T3_G_TAP, T3_G_DRAG } t3_gesture_t;

/* Classify a stroke: tap when displacement < T3_TAP_R AND duration <
 * T3_TAP_MAX_MS, else drag. */
t3_gesture_t t3_classify(int x0, int y0, int x1, int y1, uint32_t ms);

/* Point-in-rect over prims in document order; first match wins. Returns the
 * index or -1. */
int t3_hit(const t3_spec_t *s, int x, int y);

/* ---- numeric helpers ---------------------------------------------------- */

/* Clamp v into [min,max] and snap to the step grid anchored at min. */
float t3_snap(const t3_prim_t *p, float v);

/* Track extent along the primitive's axis: the usable inset span a drag maps
 * onto. For axis 'x': *origin = rect.x + inset, *len = rect.w - 2*inset. */
void t3_slider_track(const t3_prim_t *p, int *origin, int *len);

/* Map a framebuffer coordinate on the drag axis to a snapped value
 * (firmware-spec §7: t = clamp((f - origin)/len, 0, 1); for axis 'y' the top
 * of the track is max). */
float t3_slider_value_at(const t3_prim_t *p, int fx, int fy);

/* Stepper zone under a point: -1 = minus third, 0 = value third (inert),
 * +1 = plus third, and -2 when the point is outside the rect. */
int t3_stepper_zone(const t3_prim_t *p, int x, int y);

/* The zone's rect (used for the invert-zone feedback). zone is -1/0/+1. */
t3_rect_t t3_stepper_zone_rect(const t3_prim_t *p, int zone);

/* The switch's track rect (feedback repaints only this, not the label). */
t3_rect_t t3_switch_track_rect(const t3_prim_t *p, const t3_spec_t *s);

/* ---- text ---------------------------------------------------------------- */

/* Glyph lookup. t3_glyph() takes a byte pointer and consumes 1 or 2 bytes
 * (the degree sign is the only multi-byte char in the charset); *adv_bytes
 * receives how many were consumed. NULL = not in the atlas. */
const t3_glyph_t *t3_glyph(const t3_atlas_t *a, const char *p, int *adv_bytes);

/* Advance width of a string in atlas a (unknown chars count mean_w). */
int t3_text_width(const t3_atlas_t *a, const char *str);

/* Width of the widest string a value_text ref can produce: max_chars digit
 * cells plus its suffix. Keeps a value's footprint STABLE as the number moves
 * (firmware-spec §6) -- sizing off the current string would make the box twitch
 * between "9%" and "100%". */
int t3_value_box_w(const t3_atlas_t *a, const t3_text_ref_t *ref);

/* Format a numeric value for display: integral steps render as integers,
 * fractional steps to one decimal; then the ref's suffix is appended and the
 * result clipped to max_chars (footprint bound, firmware-spec §6). */
void t3_format_value(const t3_prim_t *p, const t3_text_ref_t *ref,
                     char *out, size_t cap);

/* ---- framebuffer draw ops (panel-native, bpp 4 or 1) ---------------------
 * All take ink-scale levels and invert internally. Rects are clipped to the
 * framebuffer, so a caller can pass geometry that overhangs. */

void t3_fill_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                  int x, int y, int w, int h, uint8_t ink);

/* Rounded-rect outline, `stroke` px thick, drawn INSIDE (x,y,w,h). */
void t3_round_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                   int x, int y, int w, int h, int radius, int stroke,
                   uint8_t ink);

/* Filled rounded rect (radius <= 0 => plain rect). */
void t3_fill_round_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                        int x, int y, int w, int h, int radius, uint8_t ink);

/* Filled / stroked circle centred on (cx, cy). */
void t3_fill_circle(uint8_t *fb, int fb_w, int fb_h, int bpp,
                    int cx, int cy, int d, uint8_t ink);
void t3_stroke_circle(uint8_t *fb, int fb_w, int fb_h, int bpp,
                      int cx, int cy, int d, int stroke, uint8_t ink);

/* Blit `str` into the box (x,y,w,h) honouring align; vertically centred using
 * the atlas ascent/descent. Does NOT clear the box first (callers that need a
 * clean slate fill with paper -- v3 control rects arrive blank). No-op when
 * the atlas has no bits attached. Returns the advance width drawn. */
int t3_draw_text(uint8_t *fb, int fb_w, int fb_h, int bpp,
                 const t3_atlas_t *a, const char *str,
                 int x, int y, int w, int h, t3_align_t align);

/* ---- icons (Phosphor font path) ------------------------------------------
 * The finalized contract renders button icons from a BUNDLED Phosphor weight
 * (icon.name -> codepoint via the pinned map), never from the atlas. The
 * rasterizer is not bundled yet -- it needs three things the server side has
 * not produced: a pinned Phosphor version string, a codepoint map regenerated
 * from it, and a TTF/OTF of the bold weight (only WOFF2 ships today, which a
 * TrueType rasterizer cannot read).
 *
 * This is the seam that path plugs into. Until T3_HAVE_ICON_FONT is defined the
 * renderer reports "no icon" and buttons draw label-only, which is the same
 * graceful degradation as a missing atlas -- chrome is never blank. The `icons`
 * capability stays UNADVERTISED while this is stubbed, so the server is never
 * told the device can draw something it cannot. */

/* Codepoint for a Phosphor icon name, or 0 when unknown / no map bundled. */
uint32_t t3_icon_codepoint(const char *name);

/* Rasterize `icon` into the framebuffer with its top-left at (x,y), in ink.
 * Returns the advance width drawn, or 0 when no icon could be rendered (no font
 * bundled, unknown name) -- callers must treat 0 as "lay out without an icon". */
int t3_draw_icon(uint8_t *fb, int fb_w, int fb_h, int bpp,
                 const t3_icon_ref_t *icon, int x, int y);

/* Rendered size of an icon, 0 when it cannot be drawn. Lets layout reserve the
 * right box before drawing. */
int t3_icon_width(const t3_icon_ref_t *icon);

/* ---- primitive rendering (firmware-spec §7) -----------------------------
 * Draws the primitive's chrome + content into its rect, which the server
 * left blank. Deterministic: the server's canvas preview draws the same math.
 * `s` supplies the atlases the primitive's text refs point into. */
void t3_draw_primitive(uint8_t *fb, int fb_w, int fb_h, int bpp,
                       const t3_spec_t *s, const t3_prim_t *p);

/* The sub-rect a feedback repaint has to cover for this primitive: the whole
 * rect for button/stepper, the track for a switch, the track band plus the
 * value_text strip for a slider. Keeps DU refreshes as small as the contract
 * demands ("partial-refresh the control rect only"). */
t3_rect_t t3_feedback_rect(const t3_spec_t *s, const t3_prim_t *p);

/* ---- report strings ---- */
const char *t3_interaction_name(t3_gesture_t g);   /* "tap" | "set" */
const char *t3_ptype_name(t3_ptype_t t);
