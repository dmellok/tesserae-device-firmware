/* touch3.c -- pure engine for Tesserae touch v3. See touch3.h. */

#include "touch3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- small helpers ------------------------------------------------------ */

static void cpy(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static int jint(const cJSON *o, const char *k, int dflt)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valueint : dflt;
}

static float jnum(const cJSON *o, const char *k, float dflt, bool *found)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(i)) { if (found) *found = false; return dflt; }
    if (found) *found = true;
    return (float)i->valuedouble;
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

/* ---- glyph slots ------------------------------------------------------- */

/* Slot for a single byte, or -1. The charset is printable ASCII plus the
 * degree sign, which lives in the reserved T3_GLYPH_DEGREE slot. */
static int ascii_slot(unsigned char c)
{
    if (c < T3_GLYPH_FIRST || c > T3_GLYPH_LAST) return -1;
    return (int)c - T3_GLYPH_FIRST;
}

const t3_glyph_t *t3_glyph(const t3_atlas_t *a, const char *p, int *adv_bytes)
{
    if (adv_bytes) *adv_bytes = 1;
    if (!a || !p || !*p) return NULL;
    unsigned char c = (unsigned char)p[0];

    /* U+00B0 DEGREE SIGN = 0xC2 0xB0 in UTF-8. */
    if (c == 0xC2 && (unsigned char)p[1] == 0xB0) {
        if (adv_bytes) *adv_bytes = 2;
        return a->have[T3_GLYPH_DEGREE] ? &a->glyphs[T3_GLYPH_DEGREE] : NULL;
    }
    int slot = ascii_slot(c);
    if (slot < 0) return NULL;
    return a->have[slot] ? &a->glyphs[slot] : NULL;
}

int t3_text_width(const t3_atlas_t *a, const char *str)
{
    if (!a || !str) return 0;
    int w = 0;
    for (const char *p = str; *p; ) {
        int nb = 1;
        const t3_glyph_t *g = t3_glyph(a, p, &nb);
        if (g) w += g->adv;
        else if ((unsigned char)*p == ' ') w += a->space_adv;
        else w += a->mean_w;
        p += nb;
    }
    return w;
}

int t3_value_box_w(const t3_atlas_t *a, const t3_text_ref_t *ref)
{
    if (!a || !ref) return 0;
    int chars = (ref->max_chars > 0) ? ref->max_chars : 4;
    /* Widest digit, not the mean: mean_w averages in narrow punctuation, which
     * would under-size the box and clip "100%". */
    int cell = a->digit_w > 0 ? a->digit_w : a->mean_w;
    int w = chars * cell;
    if (ref->suffix[0]) w += t3_text_width(a, ref->suffix);
    return w;
}

/* ---- parse ------------------------------------------------------------- */

static bool parse_atlas(const cJSON *o, t3_atlas_t *a)
{
    memset(a, 0, sizeof *a);
    const char *id = jstr(o, "id");
    const char *dg = jstr(o, "digest");
    const char *url = jstr(o, "url");
    const char *fmt = jstr(o, "format");
    if (!id || !dg || !url) return false;
    /* format is a const in the schema; anything else we cannot decode. */
    if (fmt && strcmp(fmt, "gray4") != 0) return false;

    cpy(a->id, sizeof a->id, id);
    cpy(a->digest, sizeof a->digest, dg);
    cpy(a->url, sizeof a->url, url);
    a->px        = jint(o, "px", 0);
    a->weight    = jint(o, "weight", 400);
    a->strip_h   = jint(o, "strip_h", 0);
    a->strip_w   = jint(o, "strip_w", 0);
    a->ascent    = jint(o, "ascent", a->strip_h);
    a->descent   = jint(o, "descent", 0);
    a->space_adv = jint(o, "space_adv", 0);
    if (a->strip_h <= 0) return false;

    const cJSON *gl = cJSON_GetObjectItemCaseSensitive(o, "glyphs");
    if (!cJSON_IsObject(gl)) return false;

    int derived_w = 0, sum_w = 0, n_w = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, gl) {
        if (!cJSON_IsObject(it) || !it->string) continue;
        t3_glyph_t g = { (uint16_t)jint(it, "x", 0),
                         (uint16_t)jint(it, "w", 0),
                         (uint16_t)jint(it, "adv", 0) };
        if (g.adv == 0 && g.w == 0) continue;          /* nothing to place */
        if ((int)(g.x + g.w) > derived_w) derived_w = g.x + g.w;
        if (g.w > 0) { sum_w += g.w; n_w++; }

        const char *key = it->string;
        int slot;
        if ((unsigned char)key[0] == 0xC2 && (unsigned char)key[1] == 0xB0)
            slot = T3_GLYPH_DEGREE;                    /* "°" */
        else if (key[1] == '\0')
            slot = ascii_slot((unsigned char)key[0]);
        else
            continue;                                  /* not a charset key */
        if (slot < 0) continue;
        a->have[slot] = true;
        a->glyphs[slot] = g;
    }
    if (a->strip_w <= 0) a->strip_w = derived_w;
    if (a->strip_w <= 0) return false;
    a->mean_w = n_w ? sum_w / n_w : 0;
    if (a->space_adv <= 0) a->space_adv = a->mean_w;
    /* Widest digit advance, for stable value-text boxes. */
    for (char c = '0'; c <= '9'; c++) {
        int s = ascii_slot((unsigned char)c);
        if (s >= 0 && a->have[s] && a->glyphs[s].adv > (uint16_t)a->digit_w)
            a->digit_w = a->glyphs[s].adv;
    }
    return true;
}

static bool parse_rect(const cJSON *o, int panel_w, int panel_h, t3_rect_t *r)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, "rect");
    if (!cJSON_IsObject(j)) return false;
    int x = jint(j, "x", -1), y = jint(j, "y", -1);
    int w = jint(j, "w", 0),  h = jint(j, "h", 0);
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if (x >= panel_w || y >= panel_h) return false;       /* fully off-panel */
    if (x + w > panel_w) w = panel_w - x;                 /* clamp */
    if (y + h > panel_h) h = panel_h - y;
    if (w <= 0 || h <= 0) return false;
    r->x = (int16_t)x; r->y = (int16_t)y;
    r->w = (int16_t)w; r->h = (int16_t)h;
    return true;
}

static t3_align_t parse_align(const char *s, t3_align_t dflt)
{
    if (!s) return dflt;
    if (strcmp(s, "left") == 0)   return T3_ALIGN_LEFT;
    if (strcmp(s, "center") == 0) return T3_ALIGN_CENTER;
    if (strcmp(s, "right") == 0)  return T3_ALIGN_RIGHT;
    return dflt;
}

/* Resolve an atlas handle ("l20") to an index in the parsed array. */
static int atlas_index(const t3_spec_t *s, const char *handle)
{
    if (!handle) return -1;
    for (int i = 0; i < s->n_atlases; i++)
        if (strcmp(s->atlases[i].id, handle) == 0) return i;
    return -1;
}

static void parse_text_ref(const cJSON *parent, const char *key,
                           const t3_spec_t *s, t3_align_t dflt_align,
                           t3_text_ref_t *out)
{
    memset(out, 0, sizeof *out);
    out->atlas_idx = -1;
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsObject(o)) return;
    out->present   = true;
    out->atlas_idx = atlas_index(s, jstr(o, "atlas"));
    out->align     = parse_align(jstr(o, "align"), dflt_align);
    cpy(out->text,   sizeof out->text,   jstr(o, "text"));
    cpy(out->suffix, sizeof out->suffix, jstr(o, "suffix"));
    out->max_chars = jint(o, "max_chars", 0);
}

static void parse_icon_ref(const cJSON *parent, t3_icon_ref_t *out)
{
    memset(out, 0, sizeof *out);
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, "icon");
    if (!cJSON_IsObject(o)) return;
    const char *name = jstr(o, "name");
    if (!name || !name[0]) return;
    out->present = true;
    cpy(out->name, sizeof out->name, name);
    out->px = jint(o, "px", T3_ICON_PX);
    if (out->px < 8) out->px = T3_ICON_PX;
    const char *w = jstr(o, "weight");
    out->duotone = w && strcmp(w, "duotone") == 0;
}

/* Parse the optional "action" block.
 *
 * The schema lists action as required for every primitive, but the LIVE server
 * omits it on value-bound controls (sliders, steppers): those act through their
 * value_key, so there is no discrete action to name. Requiring it dropped every
 * slider and stepper on a real 46-primitive page -- they parsed and then
 * vanished. Absent action therefore defaults to tier 1 + ha, which is exactly
 * the semantics a value-bound control wants: draw optimistically, report, let
 * the values stream confirm. A malformed action degrades the same way rather
 * than discarding an otherwise good primitive. */
static void parse_action(const cJSON *parent, t3_prim_t *p)
{
    p->tier  = 1;
    p->atype = T3_ACT_HA;

    const cJSON *o = cJSON_GetObjectItemCaseSensitive(parent, "action");
    if (!cJSON_IsObject(o)) return;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "tier");
    if (cJSON_IsNumber(t) && t->valueint >= 0 && t->valueint <= 2)
        p->tier = t->valueint;

    const char *ty = jstr(o, "type");
    if (!ty) return;
    if      (strcmp(ty, "nav") == 0)     p->atype = T3_ACT_NAV;
    else if (strcmp(ty, "ha") == 0)      p->atype = T3_ACT_HA;
    else if (strcmp(ty, "webhook") == 0) p->atype = T3_ACT_WEBHOOK;
    else if (strcmp(ty, "refresh") == 0) p->atype = T3_ACT_REFRESH;
    else if (strcmp(ty, "fetch") == 0)   p->atype = T3_ACT_FETCH;
    else {
        /* Newer server: keep the control but make it round-trip (never
         * optimistic on semantics we do not understand). */
        p->atype = T3_ACT_UNKNOWN;
        p->tier  = 2;
    }
}

static bool parse_primitive(const cJSON *o, int panel_w, int panel_h,
                            const t3_spec_t *s, t3_prim_t *p)
{
    memset(p, 0, sizeof *p);
    p->label.atlas_idx = p->value_text.atlas_idx = -1;

    const char *id = jstr(o, "id");
    const char *ty = jstr(o, "type");
    if (!id || !id[0] || !ty) return false;
    cpy(p->id, sizeof p->id, id);

    if      (strcmp(ty, "button") == 0)  p->type = T3_BUTTON;
    else if (strcmp(ty, "switch") == 0)  p->type = T3_SWITCH;
    else if (strcmp(ty, "slider") == 0)  p->type = T3_SLIDER;
    else if (strcmp(ty, "stepper") == 0) p->type = T3_STEPPER;
    else return false;      /* unknown type: skip this primitive (§13) */

    if (!parse_rect(o, panel_w, panel_h, &p->rect)) return false;
    parse_action(o, p);          /* optional; defaults to tier 1 + ha */

    parse_text_ref(o, "label", s,
                   p->type == T3_SWITCH ? T3_ALIGN_LEFT : T3_ALIGN_CENTER,
                   &p->label);
    parse_text_ref(o, "value_text", s, T3_ALIGN_CENTER, &p->value_text);
    parse_icon_ref(o, &p->icon);
    cpy(p->value_key, sizeof p->value_key, jstr(o, "value_key"));

    if (p->type == T3_SWITCH) {
        if (!p->value_key[0]) return false;            /* schema: required */
        const char *st = jstr(o, "state");
        p->state = st && strcmp(st, "on") == 0;
    }

    if (p->type == T3_SLIDER || p->type == T3_STEPPER) {
        bool has_min = false, has_max = false;
        p->vmin = jnum(o, "min", 0.0f, &has_min);
        p->vmax = jnum(o, "max", 0.0f, &has_max);
        if (!has_min || !has_max || !(p->vmax > p->vmin)) return false;
        p->vstep = jnum(o, "step", 1.0f, NULL);
        if (!(p->vstep > 0.0f)) p->vstep = 1.0f;
        p->value = jnum(o, "value", p->vmin, NULL);
        p->value = t3_snap(p, p->value);

        if (p->type == T3_SLIDER) {
            const char *ax = jstr(o, "axis");
            if (!ax || (ax[0] != 'x' && ax[0] != 'y')) return false;
            p->axis = ax[0];
        } else {
            p->axis = 'x';
        }
    }
    return true;
}

bool t3_spec_parse(const char *json, size_t len,
                   int panel_w, int panel_h, t3_spec_t *out)
{
    if (!json || !out || panel_w <= 0 || panel_h <= 0) return false;
    memset(out, 0, sizeof *out);

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;

    const char *ld = jstr(root, "layout_digest");
    if (!ld || !ld[0]) goto done;                     /* strict: the anchor */
    cpy(out->layout_digest, sizeof out->layout_digest, ld);

    const cJSON *ats = cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (cJSON_IsArray(ats)) {
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, ats) {
            if (out->n_atlases >= T3_MAX_ATLASES) break;
            t3_atlas_t a;
            if (!parse_atlas(it, &a)) continue;       /* drop, keep the spec */
            bool dup = false;                          /* dedupe by digest */
            for (int i = 0; i < out->n_atlases; i++)
                if (strcmp(out->atlases[i].digest, a.digest) == 0) dup = true;
            if (dup) continue;
            out->atlases[out->n_atlases++] = a;
        }
    }

    const cJSON *ps = cJSON_GetObjectItemCaseSensitive(root, "primitives");
    if (!cJSON_IsArray(ps)) goto done;                /* schema: required */
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, ps) {
        if (out->n_prims >= T3_MAX_PRIMS) break;
        t3_prim_t p;
        if (!parse_primitive(it, panel_w, panel_h, out, &p)) continue;
        out->prims[out->n_prims++] = p;
    }
    ok = true;

done:
    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

bool t3_spec_matches(const t3_spec_t *s, const char *d)
{
    return s && d && d[0] && s->layout_digest[0] &&
           strcmp(s->layout_digest, d) == 0;
}

t3_prim_t *t3_prim_by_id(t3_spec_t *s, const char *id)
{
    if (!s || !id) return NULL;
    for (int i = 0; i < s->n_prims; i++)
        if (strcmp(s->prims[i].id, id) == 0) return &s->prims[i];
    return NULL;
}

/* ---- hit test + gestures ----------------------------------------------- */

t3_gesture_t t3_classify(int x0, int y0, int x1, int y1, uint32_t ms)
{
    int dx = x1 - x0, dy = y1 - y0;
    int d2 = dx * dx + dy * dy;
    if (d2 < T3_TAP_R * T3_TAP_R && ms < T3_TAP_MAX_MS) return T3_G_TAP;
    return T3_G_DRAG;
}

static bool in_rect(const t3_rect_t *r, int x, int y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

int t3_hit(const t3_spec_t *s, int x, int y)
{
    if (!s) return -1;
    for (int i = 0; i < s->n_prims; i++)
        if (in_rect(&s->prims[i].rect, x, y)) return i;
    return -1;
}

/* ---- numeric helpers -------------------------------------------------- */

float t3_snap(const t3_prim_t *p, float v)
{
    if (!p) return v;
    if (v < p->vmin) v = p->vmin;
    if (v > p->vmax) v = p->vmax;
    float step = p->vstep > 0.0f ? p->vstep : 1.0f;
    float snapped = p->vmin + roundf((v - p->vmin) / step) * step;
    if (snapped < p->vmin) snapped = p->vmin;
    if (snapped > p->vmax) snapped = p->vmax;
    return snapped;
}

void t3_slider_track(const t3_prim_t *p, int *origin, int *len)
{
    int o = 0, l = 0;
    if (p) {
        if (p->axis == 'y') {
            o = p->rect.y + T3_SLIDER_INSET;
            l = p->rect.h - 2 * T3_SLIDER_INSET;
        } else {
            o = p->rect.x + T3_SLIDER_INSET;
            l = p->rect.w - 2 * T3_SLIDER_INSET;
        }
        if (l < 1) l = 1;
    }
    if (origin) *origin = o;
    if (len) *len = l;
}

float t3_slider_value_at(const t3_prim_t *p, int fx, int fy)
{
    if (!p) return 0.0f;
    int origin = 0, len = 1;
    t3_slider_track(p, &origin, &len);
    int f = (p->axis == 'y') ? fy : fx;
    float t = (float)(f - origin) / (float)len;
    if (p->axis == 'y') t = 1.0f - t;        /* top of the track = max */
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t3_snap(p, p->vmin + t * (p->vmax - p->vmin));
}

int t3_stepper_zone(const t3_prim_t *p, int x, int y)
{
    if (!p || !in_rect(&p->rect, x, y)) return -2;
    int third = p->rect.w / 3;
    if (third < 1) third = 1;
    int dx = x - p->rect.x;
    if (dx < third) return -1;
    if (dx >= 2 * third) return 1;
    return 0;
}

t3_rect_t t3_stepper_zone_rect(const t3_prim_t *p, int zone)
{
    t3_rect_t r = { 0, 0, 0, 0 };
    if (!p) return r;
    int third = p->rect.w / 3;
    if (third < 1) third = 1;
    r.y = p->rect.y;
    r.h = p->rect.h;
    r.w = (int16_t)third;
    if (zone < 0)       r.x = p->rect.x;
    else if (zone == 0) r.x = (int16_t)(p->rect.x + third);
    else {
        r.x = (int16_t)(p->rect.x + 2 * third);
        r.w = (int16_t)(p->rect.w - 2 * third);   /* absorb the remainder */
    }
    return r;
}

/* Label width for a switch (0 when it has no drawable label). */
static int switch_label_w(const t3_prim_t *p, const t3_spec_t *s)
{
    if (!p->label.present || p->label.atlas_idx < 0 || !s) return 0;
    if (p->label.atlas_idx >= s->n_atlases) return 0;
    const t3_atlas_t *a = &s->atlases[p->label.atlas_idx];
    if (!p->label.text[0]) return 0;
    return t3_text_width(a, p->label.text);
}

t3_rect_t t3_switch_track_rect(const t3_prim_t *p, const t3_spec_t *s)
{
    t3_rect_t r = { 0, 0, 0, 0 };
    if (!p) return r;
    int lw = switch_label_w(p, s);
    int off = lw ? lw + T3_SWITCH_GAP : 0;
    int th = imin(p->rect.h, T3_SWITCH_TRACK_H);
    int cw = p->rect.w - off;
    if (cw < 2) { off = 0; cw = p->rect.w; }        /* label crowds it out */
    r.x = (int16_t)(p->rect.x + off);
    r.y = (int16_t)(p->rect.y + (p->rect.h - th) / 2);
    r.w = (int16_t)cw;
    r.h = (int16_t)th;
    return r;
}

/* ---- value formatting ------------------------------------------------- */

void t3_format_value(const t3_prim_t *p, const t3_text_ref_t *ref,
                     char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!p) return;

    char num[16];
    float step = p->vstep > 0.0f ? p->vstep : 1.0f;
    bool integral = fabsf(step - roundf(step)) < 0.001f &&
                    fabsf(p->value - roundf(p->value)) < 0.001f;
    if (integral) snprintf(num, sizeof num, "%d", (int)lroundf(p->value));
    else          snprintf(num, sizeof num, "%.1f", (double)p->value);

    const char *suffix = (ref && ref->suffix[0]) ? ref->suffix : "";
    snprintf(out, cap, "%s%s", num, suffix);

    /* max_chars bounds the FOOTPRINT so layout stays stable as the number
     * moves (firmware-spec §6). The suffix is part of that footprint, so
     * clipping counts characters, not bytes -- the degree sign is 2 bytes. */
    if (ref && ref->max_chars > 0) {
        int chars = 0;
        char *q = out;
        while (*q) {
            int nb = ((unsigned char)*q == 0xC2) ? 2 : 1;
            if (chars >= ref->max_chars) { *q = '\0'; break; }
            chars++;
            q += nb;
        }
    }
}

/* ---- framebuffer primitives ------------------------------------------- */

/* 4bpp nibble accessors (2 px/byte, HIGH nibble = left pixel), matching
 * proto2.c / overlay.c. */
static inline void px_set4(uint8_t *fb, int stride, int x, int y, uint8_t v)
{
    uint8_t *b = &fb[(size_t)y * stride + (x >> 1)];
    if (x & 1) *b = (uint8_t)((*b & 0xF0) | (v & 0x0F));
    else       *b = (uint8_t)((*b & 0x0F) | (uint8_t)(v << 4));
}

static inline uint8_t px_get4(const uint8_t *fb, int stride, int x, int y)
{
    uint8_t b = fb[(size_t)y * stride + (x >> 1)];
    return (x & 1) ? (b & 0x0F) : (uint8_t)(b >> 4);
}

/* 1bpp: MSB = leftmost pixel, 1 = white (mono_spi convention). */
static inline void px_set1(uint8_t *fb, int stride, int x, int y, bool white)
{
    uint8_t *b = &fb[(size_t)y * stride + (x >> 3)];
    uint8_t m = (uint8_t)(0x80 >> (x & 7));
    if (white) *b |= m; else *b = (uint8_t)(*b & ~m);
}

/* 1bpp CHROME collapse. Two inks means the four-level palette has to fold, and
 * which way each level folds is a design decision, not a threshold:
 *
 *   paper  0 -> white
 *   soft   3 -> BLACK. soft draws separators (the stepper's dividers); one that
 *               folded to paper would vanish completely, which reads as broken.
 *               Drawing it at full strength merely reads as unstyled.
 *   mid    8 -> BLACK. Keeps the "filled" cue that distinguishes a switch's on
 *               track and a slider's active portion. Callers stacking ink ON a
 *               mid fill must ask for paper instead at 1bpp, or the two collapse
 *               together and the upper shape disappears (see draw_switch).
 *   ink   15 -> black
 *
 * Duotone never reaches here: the contract gates it on panel.grayscale, and a
 * 1bpp panel gets a solid bold weight from the server instead. */
static inline bool mono_white(uint8_t ink) { return ink == T3_INK_PAPER; }

/* Write one pixel with an INK level, translating to the panel's scale. */
static inline void put_ink(uint8_t *fb, int fb_w, int bpp, int x, int y,
                          uint8_t ink)
{
    if (bpp == 1) px_set1(fb, (fb_w + 7) / 8, x, y, mono_white(ink));
    else          px_set4(fb, fb_w / 2, x, y, T3_FB(ink));
}

/* Clip (x,y,w,h) to the framebuffer. False when nothing is left. */
static bool clip(int fb_w, int fb_h, int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > fb_w) *w = fb_w - *x;
    if (*y + *h > fb_h) *h = fb_h - *y;
    return *w > 0 && *h > 0;
}

void t3_fill_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                  int x, int y, int w, int h, uint8_t ink)
{
    if (!fb || !clip(fb_w, fb_h, &x, &y, &w, &h)) return;
    /* Whole-byte fast path for aligned 4bpp spans (control rects are large). */
    if (bpp == 4 && (x & 1) == 0 && (w & 1) == 0) {
        uint8_t n = T3_FB(ink);
        uint8_t byte = (uint8_t)((n << 4) | n);
        int stride = fb_w / 2;
        for (int r = 0; r < h; r++)
            memset(fb + (size_t)(y + r) * stride + x / 2, byte, (size_t)w / 2);
        return;
    }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            put_ink(fb, fb_w, bpp, x + c, y + r, ink);
}

/* Horizontal extent of a rounded rect at row dy (0..h-1), as an inset from
 * each side. Uses a circle test on the corner arcs. */
static int rr_inset(int h, int dy, int radius)
{
    if (radius <= 0) return 0;
    int from_edge = dy < radius ? radius - dy
                  : (dy >= h - radius ? dy - (h - radius) + 1 : 0);
    if (from_edge <= 0) return 0;
    /* dx such that dx^2 + from_edge^2 <= radius^2 */
    int rem = radius * radius - from_edge * from_edge;
    if (rem < 0) return radius;
    int dx = 0;
    while ((dx + 1) * (dx + 1) <= rem) dx++;
    return radius - dx;
}

void t3_fill_round_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                        int x, int y, int w, int h, int radius, uint8_t ink)
{
    if (!fb || w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    for (int dy = 0; dy < h; dy++) {
        int in = rr_inset(h, dy, radius);
        t3_fill_rect(fb, fb_w, fb_h, bpp, x + in, y + dy, w - 2 * in, 1, ink);
    }
}

void t3_round_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                   int x, int y, int w, int h, int radius, int stroke,
                   uint8_t ink)
{
    if (!fb || w <= 0 || h <= 0) return;
    if (stroke < 1) stroke = 1;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    /* Per-row spans: outer inset from the arc, inner inset from the arc of
     * the rect shrunk by `stroke`. Rows inside the straight section draw two
     * vertical bands; the cap rows draw the full span. */
    int ih = h - 2 * stroke;
    int ir = radius - stroke;
    if (ir < 0) ir = 0;
    for (int dy = 0; dy < h; dy++) {
        int out_in = rr_inset(h, dy, radius);
        int x0 = x + out_in, span = w - 2 * out_in;
        if (span <= 0) continue;
        int idy = dy - stroke;
        if (ih <= 0 || idy < 0 || idy >= ih) {          /* solid cap row */
            t3_fill_rect(fb, fb_w, fb_h, bpp, x0, y + dy, span, 1, ink);
            continue;
        }
        int in_in = rr_inset(ih, idy, ir);
        int ix0 = x + stroke + in_in, ispan = (w - 2 * stroke) - 2 * in_in;
        if (ispan <= 0) {
            t3_fill_rect(fb, fb_w, fb_h, bpp, x0, y + dy, span, 1, ink);
            continue;
        }
        t3_fill_rect(fb, fb_w, fb_h, bpp, x0, y + dy, ix0 - x0, 1, ink);
        int right = x0 + span;
        t3_fill_rect(fb, fb_w, fb_h, bpp, ix0 + ispan, y + dy,
                     right - (ix0 + ispan), 1, ink);
    }
}

void t3_fill_circle(uint8_t *fb, int fb_w, int fb_h, int bpp,
                    int cx, int cy, int d, uint8_t ink)
{
    if (!fb || d <= 0) return;
    int r = d / 2;
    /* Even diameters have no centre pixel: bias the span so the drawn extent
     * is exactly d px, matching the server's canvas circle. */
    int x0 = cx - r, y0 = cy - r;
    for (int dy = 0; dy < d; dy++) {
        int oy = dy - r;
        int rem = r * r - oy * oy;
        if (rem < 0) continue;
        int dx = 0;
        while ((dx + 1) * (dx + 1) <= rem) dx++;
        t3_fill_rect(fb, fb_w, fb_h, bpp, cx - dx, y0 + dy, 2 * dx + 1, 1, ink);
    }
    (void)x0;
}

void t3_stroke_circle(uint8_t *fb, int fb_w, int fb_h, int bpp,
                      int cx, int cy, int d, int stroke, uint8_t ink)
{
    if (!fb || d <= 0) return;
    if (stroke < 1) stroke = 1;
    int ro = d / 2, ri = ro - stroke;
    if (ri < 0) ri = 0;
    for (int dy = -ro; dy <= ro; dy++) {
        int remo = ro * ro - dy * dy;
        if (remo < 0) continue;
        int dxo = 0;
        while ((dxo + 1) * (dxo + 1) <= remo) dxo++;
        int remi = ri * ri - dy * dy;
        int dxi = -1;
        if (remi >= 0) { dxi = 0; while ((dxi + 1) * (dxi + 1) <= remi) dxi++; }
        if (dxi < 0) {
            t3_fill_rect(fb, fb_w, fb_h, bpp, cx - dxo, cy + dy,
                         2 * dxo + 1, 1, ink);
        } else {
            t3_fill_rect(fb, fb_w, fb_h, bpp, cx - dxo, cy + dy,
                         dxo - dxi, 1, ink);
            t3_fill_rect(fb, fb_w, fb_h, bpp, cx + dxi + 1, cy + dy,
                         dxo - dxi, 1, ink);
        }
    }
}

/* ---- text / icon blit --------------------------------------------------- */

/* Blit one glyph slice from the strip. The strip is INK scale, so each nibble
 * inverts on the way into the framebuffer. */
static void blit_glyph(uint8_t *fb, int fb_w, int fb_h, int bpp,
                       const t3_atlas_t *a, const t3_glyph_t *g,
                       int dx, int dy, int clip_right, int clip_bottom)
{
    int st_stride = (a->strip_w + 1) / 2;
    for (int gx = 0; gx < g->w; gx++) {
        int x = dx + gx;
        if (x >= clip_right) break;
        if (x < 0 || x >= fb_w) continue;
        for (int gy = 0; gy < a->strip_h; gy++) {
            int y = dy + gy;
            if (y >= clip_bottom) break;
            if (y < 0 || y >= fb_h) continue;
            if (g->x + gx >= a->strip_w) continue;
            uint8_t ink = px_get4(a->bits, st_stride, g->x + gx, gy);
            if (ink == T3_INK_PAPER) continue;      /* transparent paper */
            /* Glyph coverage is CONTINUOUS TONE, unlike chrome, so 1bpp
             * thresholds it at the midpoint instead of folding every non-paper
             * level to black -- otherwise each antialiased edge pixel inks and
             * every glyph renders a stroke heavier than it should. */
            if (bpp == 1 && ink < T3_INK_MID) continue;
            put_ink(fb, fb_w, bpp, x, y, ink);
        }
    }
}

int t3_draw_text(uint8_t *fb, int fb_w, int fb_h, int bpp,
                 const t3_atlas_t *a, const char *str,
                 int x, int y, int w, int h, t3_align_t align)
{
    if (!fb || !a || !a->bits || !str || !str[0] || w <= 0 || h <= 0) return 0;

    int total = t3_text_width(a, str);
    int pen = x;
    if (align == T3_ALIGN_CENTER)     pen = x + (w - total) / 2;
    else if (align == T3_ALIGN_RIGHT) pen = x + w - total;
    if (pen < x) pen = x;

    /* Vertical placement (firmware-spec §6): the INK band is ascent+descent
     * tall and gets centred in the box; the slice actually blitted is the
     * full strip_h, anchored at baseline-ascent == the band's top. Nudge the
     * slice back inside the box when it would overhang, so text never bleeds
     * into neighbouring chrome (an exact-fit box then draws flush at y). */
    int band = a->ascent + a->descent;
    if (band <= 0 || band > a->strip_h) band = a->strip_h;
    int top = y + (h - band) / 2;
    if (top + a->strip_h > y + h) top = y + h - a->strip_h;
    if (top < y) top = y;

    int clip_right = x + w;
    int clip_bottom = y + h;
    for (const char *p = str; *p; ) {
        int nb = 1;
        const t3_glyph_t *g = t3_glyph(a, p, &nb);
        if (!g) {
            pen += ((unsigned char)*p == ' ') ? a->space_adv : a->mean_w;
        } else {
            blit_glyph(fb, fb_w, fb_h, bpp, a, g, pen, top, clip_right,
                       clip_bottom);
            pen += g->adv;
        }
        p += nb;
        if (pen >= clip_right) break;
    }
    return total;
}

/* ---- icons ---------------------------------------------------------------
 * The real implementations live in touch3_icons.c, which owns the embedded
 * Phosphor font and the rasterizer. These stubs cover builds without the font
 * (boards that do not need it, and the host test binary): buttons then lay out
 * label-only, the same degradation as a missing atlas. */
#ifndef T3_HAVE_ICON_FONT

uint32_t t3_icon_codepoint(const char *name)
{
    (void)name;
    return 0;
}

int t3_icon_width(const t3_icon_ref_t *icon)
{
    (void)icon;
    return 0;      /* no font bundled: layout omits the icon entirely */
}

int t3_draw_icon(uint8_t *fb, int fb_w, int fb_h, int bpp,
                 const t3_icon_ref_t *icon, int x, int y)
{
    (void)fb; (void)fb_w; (void)fb_h; (void)bpp; (void)icon; (void)x; (void)y;
    return 0;
}

#endif /* !T3_HAVE_ICON_FONT */

/* ---- primitive rendering ---------------------------------------------- */

static const t3_atlas_t *ref_atlas(const t3_spec_t *s, const t3_text_ref_t *r)
{
    if (!s || !r || !r->present) return NULL;
    if (r->atlas_idx < 0 || r->atlas_idx >= s->n_atlases) return NULL;
    const t3_atlas_t *a = &s->atlases[r->atlas_idx];
    return a->bits ? a : NULL;
}

static void draw_button(uint8_t *fb, int fb_w, int fb_h, int bpp,
                        const t3_spec_t *s, const t3_prim_t *p)
{
    const t3_rect_t *R = &p->rect;
    /* Frame: fill paper, then a rounded outline inset by stroke/2 so the
     * stroke lands fully inside the rect (firmware-spec §7). */
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x, R->y, R->w, R->h, T3_INK_PAPER);
    int in = T3_STROKE / 2;
    t3_round_rect(fb, fb_w, fb_h, bpp, R->x + in, R->y + in,
                  R->w - 2 * in, R->h - 2 * in,
                  T3_CORNER_RADIUS, T3_STROKE, T3_INK_INK);

    /* Content: icon + gap + label, whichever are present, centred in R. */
    const t3_atlas_t *la = ref_atlas(s, &p->label);
    const char *text = p->label.text[0] ? p->label.text : NULL;
    int label_w = (la && text) ? t3_text_width(la, text) : 0;

    /* Icon from the bundled Phosphor font. 0 when no font is bundled or the
     * name is unknown, in which case the layout collapses to label-only rather
     * than reserving an empty gap. */
    int icon_w = t3_icon_width(&p->icon);
    int icon_h = icon_w;                  /* Phosphor cells are square */

    int gap = (icon_w && label_w) ? T3_GAP : 0;
    int total = icon_w + gap + label_w;
    int cx = R->x + (R->w - total) / 2;
    if (cx < R->x + T3_PAD) cx = R->x + T3_PAD;

    if (icon_w) {
        t3_draw_icon(fb, fb_w, fb_h, bpp, &p->icon, cx,
                     R->y + (R->h - icon_h) / 2);
        cx += icon_w + gap;
    }
    if (label_w)
        t3_draw_text(fb, fb_w, fb_h, bpp, la, text,
                     cx, R->y, label_w, R->h, T3_ALIGN_LEFT);
}

static void draw_switch(uint8_t *fb, int fb_w, int fb_h, int bpp,
                        const t3_spec_t *s, const t3_prim_t *p)
{
    const t3_rect_t *R = &p->rect;
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x, R->y, R->w, R->h, T3_INK_PAPER);

    /* Label sits to the left of the control area. */
    const t3_atlas_t *la = ref_atlas(s, &p->label);
    if (la && p->label.text[0]) {
        int lw = t3_text_width(la, p->label.text);
        t3_draw_text(fb, fb_w, fb_h, bpp, la, p->label.text,
                     R->x, R->y, lw, R->h, T3_ALIGN_LEFT);
    }

    t3_rect_t tr = t3_switch_track_rect(p, s);
    int th = tr.h, radius = th / 2;

    /* Track: pill, filled paper (off) or mid (on), then stroked. */
    t3_fill_round_rect(fb, fb_w, fb_h, bpp, tr.x, tr.y, tr.w, tr.h, radius,
                       p->state ? T3_INK_MID : T3_INK_PAPER);
    t3_round_rect(fb, fb_w, fb_h, bpp, tr.x, tr.y, tr.w, tr.h, radius,
                  T3_STROKE, T3_INK_INK);

    /* Thumb: ink circle, d = track height - 2*inset, parked at either end. */
    int d = th - 2 * T3_SWITCH_INSET;
    if (d < 2) d = 2;
    int cy = tr.y + th / 2;
    int cx = p->state ? (tr.x + tr.w - T3_SWITCH_INSET - d / 2)
                      : (tr.x + T3_SWITCH_INSET + d / 2);
    /* At 1bpp the on-track's mid fill collapses to solid black, so an ink thumb
     * on top of it would be invisible -- exactly the shape that carries the
     * state. Draw it in paper there: a white thumb on a black track, which is
     * how two-colour toggles read anyway. Grayscale panels keep ink-on-mid. */
    uint8_t thumb_ink = (bpp == 1 && p->state) ? T3_INK_PAPER : T3_INK_INK;
    t3_fill_circle(fb, fb_w, fb_h, bpp, cx, cy, d, thumb_ink);
}

/* Where the slider's value_text goes: a band above the thumb, clamped inside
 * the rect (firmware-spec §7). Returns false when there is no text to draw. */
static bool slider_vtext_box(const t3_spec_t *s, const t3_prim_t *p,
                             const t3_atlas_t **out_a, t3_rect_t *box)
{
    const t3_atlas_t *a = ref_atlas(s, &p->value_text);
    if (!a) return false;
    const t3_rect_t *R = &p->rect;
    int bh = a->strip_h;
    int bw = R->w;
    int bx = R->x;
    int by = R->y;

#if T3_SLIDER_VTEXT_ABOVE_THUMB
    if (p->axis == 'x') {
        int origin = 0, len = 1;
        t3_slider_track(p, &origin, &len);
        float t = (p->vmax > p->vmin)
                ? (p->value - p->vmin) / (p->vmax - p->vmin) : 0.0f;
        int cx = origin + (int)lroundf(t * (float)len);
        bw = t3_value_box_w(a, &p->value_text) + 2 * T3_GAP;
        if (bw < 1) bw = 1;
        bx = cx - bw / 2;
        if (bx < R->x) bx = R->x;
        if (bx + bw > R->x + R->w) bx = R->x + R->w - bw;
    }
#endif
    if (bh > R->h) bh = R->h;
    if (bw < 1) bw = 1;
    box->x = (int16_t)bx; box->y = (int16_t)by;
    box->w = (int16_t)bw; box->h = (int16_t)bh;
    if (out_a) *out_a = a;
    return true;
}

/* Vertical band the slider's track + thumb occupy (axis x), or the
 * horizontal band (axis y). */
static t3_rect_t slider_track_band(const t3_spec_t *s, const t3_prim_t *p)
{
    const t3_rect_t *R = &p->rect;
    t3_rect_t band = *R;
    const t3_atlas_t *va = ref_atlas(s, &p->value_text);
    int reserve = va ? va->strip_h : 0;
    if (p->axis == 'x' && reserve > 0 && R->h - reserve >= T3_SLIDER_THUMB_D) {
        band.y = (int16_t)(R->y + reserve);
        band.h = (int16_t)(R->h - reserve);
    }
    return band;
}

static void draw_slider(uint8_t *fb, int fb_w, int fb_h, int bpp,
                        const t3_spec_t *s, const t3_prim_t *p)
{
    const t3_rect_t *R = &p->rect;
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x, R->y, R->w, R->h, T3_INK_PAPER);

    int origin = 0, len = 1;
    t3_slider_track(p, &origin, &len);
    float t = (p->vmax > p->vmin)
            ? (p->value - p->vmin) / (p->vmax - p->vmin) : 0.0f;
    int pos = origin + (int)lroundf(t * (float)len);

    t3_rect_t band = slider_track_band(s, p);

    if (p->axis == 'x') {
        int ty = band.y + (band.h - T3_SLIDER_THICK) / 2;
        /* Rest, then the active fill from the track start to the thumb. */
        t3_fill_round_rect(fb, fb_w, fb_h, bpp, origin, ty, len,
                           T3_SLIDER_THICK, T3_SLIDER_RADIUS, T3_INK_PAPER);
        t3_round_rect(fb, fb_w, fb_h, bpp, origin, ty, len, T3_SLIDER_THICK,
                      T3_SLIDER_RADIUS, 1, T3_INK_INK);
        if (pos > origin)
            t3_fill_round_rect(fb, fb_w, fb_h, bpp, origin, ty, pos - origin,
                               T3_SLIDER_THICK, T3_SLIDER_RADIUS, T3_INK_MID);
        /* Thumb: paper fill + ink stroke, so the track reads through. */
        int cy = ty + T3_SLIDER_THICK / 2;
        t3_fill_circle(fb, fb_w, fb_h, bpp, pos, cy, T3_SLIDER_THUMB_D,
                       T3_INK_PAPER);
        t3_stroke_circle(fb, fb_w, fb_h, bpp, pos, cy, T3_SLIDER_THUMB_D,
                         T3_STROKE, T3_INK_INK);
    } else {
        int tx = band.x + (band.w - T3_SLIDER_THICK) / 2;
        t3_fill_round_rect(fb, fb_w, fb_h, bpp, tx, origin, T3_SLIDER_THICK,
                           len, T3_SLIDER_RADIUS, T3_INK_PAPER);
        t3_round_rect(fb, fb_w, fb_h, bpp, tx, origin, T3_SLIDER_THICK, len,
                      T3_SLIDER_RADIUS, 1, T3_INK_INK);
        /* Vertical fills upward from the bottom: top of the track = max. */
        int fill_top = origin + len - (int)lroundf(t * (float)len);
        if (fill_top < origin + len)
            t3_fill_round_rect(fb, fb_w, fb_h, bpp, tx, fill_top,
                               T3_SLIDER_THICK, origin + len - fill_top,
                               T3_SLIDER_RADIUS, T3_INK_MID);
        int cy = origin + len - (int)lroundf(t * (float)len);
        int cx = tx + T3_SLIDER_THICK / 2;
        t3_fill_circle(fb, fb_w, fb_h, bpp, cx, cy, T3_SLIDER_THUMB_D,
                       T3_INK_PAPER);
        t3_stroke_circle(fb, fb_w, fb_h, bpp, cx, cy, T3_SLIDER_THUMB_D,
                         T3_STROKE, T3_INK_INK);
    }

    const t3_atlas_t *va = NULL;
    t3_rect_t vb;
    if (slider_vtext_box(s, p, &va, &vb)) {
        char buf[T3_VALTEXT_CAP];
        t3_format_value(p, &p->value_text, buf, sizeof buf);
        t3_fill_rect(fb, fb_w, fb_h, bpp, vb.x, vb.y, vb.w, vb.h,
                     T3_INK_PAPER);
        t3_draw_text(fb, fb_w, fb_h, bpp, va, buf, vb.x, vb.y, vb.w, vb.h,
                     p->value_text.align);
    }
}

static void draw_stepper(uint8_t *fb, int fb_w, int fb_h, int bpp,
                         const t3_spec_t *s, const t3_prim_t *p)
{
    const t3_rect_t *R = &p->rect;
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x, R->y, R->w, R->h, T3_INK_PAPER);
    int in = T3_STROKE / 2;
    t3_round_rect(fb, fb_w, fb_h, bpp, R->x + in, R->y + in,
                  R->w - 2 * in, R->h - 2 * in,
                  T3_CORNER_RADIUS, T3_STROKE, T3_INK_INK);

    int third = R->w / 3;
    if (third < 1) third = 1;
    /* Dividers at 1/3 and 2/3, soft ink. */
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x + third, R->y + T3_STROKE,
                 T3_STROKE, R->h - 2 * T3_STROKE, T3_INK_SOFT);
    t3_fill_rect(fb, fb_w, fb_h, bpp, R->x + 2 * third, R->y + T3_STROKE,
                 T3_STROKE, R->h - 2 * T3_STROKE, T3_INK_SOFT);

    /* Marks: minus = a horizontal bar, plus = a cross. */
    int mlen = third * T3_STEP_MARK_FRAC_NUM / T3_STEP_MARK_FRAC_DEN;
    if (mlen < 2) mlen = 2;
    int cy = R->y + R->h / 2;
    int mcx = R->x + third / 2;
    int pcx = R->x + 2 * third + (R->w - 2 * third) / 2;
    int st = T3_STROKE_BOLD;
    t3_fill_rect(fb, fb_w, fb_h, bpp, mcx - mlen / 2, cy - st / 2, mlen, st,
                 T3_INK_INK);
    t3_fill_rect(fb, fb_w, fb_h, bpp, pcx - mlen / 2, cy - st / 2, mlen, st,
                 T3_INK_INK);
    t3_fill_rect(fb, fb_w, fb_h, bpp, pcx - st / 2, cy - mlen / 2, st, mlen,
                 T3_INK_INK);

    /* value_text centred in the middle third. */
    const t3_atlas_t *va = ref_atlas(s, &p->value_text);
    if (va) {
        char buf[T3_VALTEXT_CAP];
        t3_format_value(p, &p->value_text, buf, sizeof buf);
        t3_draw_text(fb, fb_w, fb_h, bpp, va, buf,
                     R->x + third + T3_STROKE, R->y,
                     third - 2 * T3_STROKE, R->h,
                     p->value_text.align);
    }
}

void t3_draw_primitive(uint8_t *fb, int fb_w, int fb_h, int bpp,
                       const t3_spec_t *s, const t3_prim_t *p)
{
    if (!fb || !s || !p) return;
    if (bpp != 4 && bpp != 1) return;
    switch (p->type) {
    case T3_BUTTON:  draw_button(fb, fb_w, fb_h, bpp, s, p);  break;
    case T3_SWITCH:  draw_switch(fb, fb_w, fb_h, bpp, s, p);  break;
    case T3_SLIDER:  draw_slider(fb, fb_w, fb_h, bpp, s, p);  break;
    case T3_STEPPER: draw_stepper(fb, fb_w, fb_h, bpp, s, p); break;
    }
}

t3_rect_t t3_feedback_rect(const t3_spec_t *s, const t3_prim_t *p)
{
    t3_rect_t r = { 0, 0, 0, 0 };
    if (!p) return r;
    switch (p->type) {
    case T3_SWITCH:
        return t3_switch_track_rect(p, s);
    case T3_SLIDER: {
        /* Track band plus the value_text strip: both move on a drag. */
        t3_rect_t band = slider_track_band(s, p);
        const t3_atlas_t *va = NULL;
        t3_rect_t vb;
        if (slider_vtext_box(s, p, &va, &vb)) {
            int y0 = imin(band.y, vb.y);
            int y1 = imax(band.y + band.h, vb.y + vb.h);
            int x0 = imin(band.x, vb.x);
            int x1 = imax(band.x + band.w, vb.x + vb.w);
            band.x = (int16_t)x0; band.y = (int16_t)y0;
            band.w = (int16_t)(x1 - x0); band.h = (int16_t)(y1 - y0);
        }
        return band;
    }
    case T3_BUTTON:
    case T3_STEPPER:
    default:
        return p->rect;
    }
}

/* ---- names ------------------------------------------------------------- */

const char *t3_interaction_name(t3_gesture_t g)
{
    return g == T3_G_DRAG ? "set" : "tap";
}

const char *t3_ptype_name(t3_ptype_t t)
{
    switch (t) {
    case T3_BUTTON:  return "button";
    case T3_SWITCH:  return "switch";
    case T3_SLIDER:  return "slider";
    case T3_STEPPER: return "stepper";
    }
    return "?";
}
