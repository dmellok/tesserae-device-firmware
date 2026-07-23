/* overlay.c -- pure overlay logic. See overlay.h. Host-testable: no ESP-IDF. */

#include "overlay.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ---------- small helpers ---------- */

static bool copy_str(char *dst, size_t cap, const cJSON *item)
{
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
        return false;
    if (strlen(item->valuestring) >= cap) return false;
    strcpy(dst, item->valuestring);
    return true;
}

static bool digest16_ok(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < OVERLAY_DIGEST_HEX; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[OVERLAY_DIGEST_HEX] == '\0';
}

/* Read x/y/w/h; clamp to panel. False = malformed or fully off-panel. */
static bool parse_rect(const cJSON *o, int pw, int ph,
                       int *x, int *y, int *w, int *h)
{
    const cJSON *jx = cJSON_GetObjectItemCaseSensitive(o, "x");
    const cJSON *jy = cJSON_GetObjectItemCaseSensitive(o, "y");
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(o, "w");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(o, "h");
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) ||
        !cJSON_IsNumber(jw) || !cJSON_IsNumber(jh)) return false;
    int rx = (int)jx->valuedouble, ry = (int)jy->valuedouble;
    int rw = (int)jw->valuedouble, rh = (int)jh->valuedouble;
    if (rw <= 0 || rh <= 0) return false;
    /* Never draw outside the panel: clamp, drop if nothing remains. */
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > pw) rw = pw - rx;
    if (ry + rh > ph) rh = ph - ry;
    if (rw <= 0 || rh <= 0) return false;
    *x = rx; *y = ry; *w = rw; *h = rh;
    return true;
}

/* ---------- spec parse ---------- */

bool overlay_spec_parse(const char *json, size_t len,
                        int panel_w, int panel_h, overlay_spec_t *out)
{
    memset(out, 0, sizeof *out);
    if (!json || !len || panel_w <= 0 || panel_h <= 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ok = false;
    do {
        const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
        if (!cJSON_IsNumber(schema) || (int)schema->valuedouble != OVERLAY_SCHEMA)
            break;
        if (!copy_str(out->frame_digest, sizeof out->frame_digest,
                      cJSON_GetObjectItemCaseSensitive(root, "frame_digest")) ||
            !digest16_ok(out->frame_digest))
            break;

        /* Atlases first so slots can validate their references. Each list is
         * independent and optional (rect-only specs are valid). */
        const cJSON *atlases = cJSON_GetObjectItemCaseSensitive(root, "atlases");
        if (cJSON_IsArray(atlases)) {
            const cJSON *ja;
            cJSON_ArrayForEach(ja, atlases) {
                if (out->n_atlases >= OVERLAY_MAX_ATLASES) break;
                overlay_atlas_t *a = &out->atlases[out->n_atlases];
                memset(a, 0, sizeof *a);
                if (!copy_str(a->id, sizeof a->id,
                              cJSON_GetObjectItemCaseSensitive(ja, "id")) ||
                    !copy_str(a->digest, sizeof a->digest,
                              cJSON_GetObjectItemCaseSensitive(ja, "digest")) ||
                    !digest16_ok(a->digest) ||
                    !copy_str(a->url, sizeof a->url,
                              cJSON_GetObjectItemCaseSensitive(ja, "url")))
                    continue;   /* drop malformed atlas */
                const cJSON *jh = cJSON_GetObjectItemCaseSensitive(ja, "height");
                if (!cJSON_IsNumber(jh) || (int)jh->valuedouble <= 0) continue;
                a->height = (int)jh->valuedouble;

                char fmt[16] = {0};
                copy_str(fmt, sizeof fmt,
                         cJSON_GetObjectItemCaseSensitive(ja, "format"));
                if (strcmp(fmt, "4bpp-gray") == 0)   a->bpp = 4;
                else if (strcmp(fmt, "1bpp") == 0)   a->bpp = 1;
                else continue;   /* unknown format: drop */

                const cJSON *glyphs = cJSON_GetObjectItemCaseSensitive(ja, "glyphs");
                if (!cJSON_IsObject(glyphs)) continue;
                long wsum = 0;
                const cJSON *jg;
                cJSON_ArrayForEach(jg, glyphs) {
                    if (a->n_glyphs >= OVERLAY_MAX_GLYPHS) break;
                    /* key must be a single-byte char */
                    if (!jg->string || strlen(jg->string) != 1) continue;
                    const cJSON *gx = cJSON_GetObjectItemCaseSensitive(jg, "x");
                    const cJSON *gw = cJSON_GetObjectItemCaseSensitive(jg, "w");
                    if (!cJSON_IsNumber(gx) || !cJSON_IsNumber(gw)) continue;
                    int vx = (int)gx->valuedouble, vw = (int)gw->valuedouble;
                    if (vx < 0 || vw <= 0 || vx + vw > 0xFFFF) continue;
                    overlay_glyph_t *g = &a->glyphs[a->n_glyphs++];
                    g->ch = jg->string[0];
                    g->x = (uint16_t)vx;
                    g->w = (uint16_t)vw;
                    wsum += vw;
                    if (vx + vw > a->strip_w) a->strip_w = vx + vw;
                }
                if (a->n_glyphs == 0) continue;   /* atlas with no glyphs: drop */
                a->mean_w = (int)(wsum / a->n_glyphs);
                if (a->mean_w < 1) a->mean_w = 1;
                out->n_atlases++;
            }
        }

        const cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
        if (cJSON_IsArray(targets)) {
            const cJSON *jt;
            cJSON_ArrayForEach(jt, targets) {
                if (out->n_targets >= OVERLAY_MAX_TARGETS) break;
                overlay_target_t *t = &out->targets[out->n_targets];
                memset(t, 0, sizeof *t);
                if (!copy_str(t->id, sizeof t->id,
                              cJSON_GetObjectItemCaseSensitive(jt, "id")))
                    continue;
                if (!parse_rect(jt, panel_w, panel_h, &t->x, &t->y, &t->w, &t->h))
                    continue;
                char echo[12] = {0};
                copy_str(echo, sizeof echo,
                         cJSON_GetObjectItemCaseSensitive(jt, "echo"));
                if (strcmp(echo, "invert") != 0) continue;   /* schema 1: invert only */
                t->echo = OVERLAY_ECHO_INVERT;
                out->n_targets++;
            }
        }

        const cJSON *slots = cJSON_GetObjectItemCaseSensitive(root, "slots");
        if (cJSON_IsArray(slots)) {
            const cJSON *js;
            cJSON_ArrayForEach(js, slots) {
                if (out->n_slots >= OVERLAY_MAX_SLOTS) break;
                overlay_slot_t *sl = &out->slots[out->n_slots];
                memset(sl, 0, sizeof *sl);
                if (!copy_str(sl->id, sizeof sl->id,
                              cJSON_GetObjectItemCaseSensitive(js, "id")) ||
                    !copy_str(sl->key, sizeof sl->key,
                              cJSON_GetObjectItemCaseSensitive(js, "key")) ||
                    !copy_str(sl->atlas_id, sizeof sl->atlas_id,
                              cJSON_GetObjectItemCaseSensitive(js, "atlas")))
                    continue;
                if (!parse_rect(js, panel_w, panel_h, &sl->x, &sl->y, &sl->w, &sl->h))
                    continue;
                /* Slot must reference a parsed atlas. */
                if (!overlay_find_atlas(out, sl->atlas_id)) continue;
                char al[12] = {0};
                copy_str(al, sizeof al, cJSON_GetObjectItemCaseSensitive(js, "align"));
                sl->align = (strcmp(al, "right") == 0)  ? OVERLAY_ALIGN_RIGHT
                          : (strcmp(al, "center") == 0) ? OVERLAY_ALIGN_CENTER
                                                        : OVERLAY_ALIGN_LEFT;
                out->n_slots++;
            }
        }

        /* A spec with nothing usable in it is not a spec. */
        ok = (out->n_targets + out->n_slots) > 0;
    } while (0);

    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

bool overlay_spec_matches(const overlay_spec_t *s, const char *d)
{
    return s && d && s->frame_digest[0] && strcmp(s->frame_digest, d) == 0;
}

const overlay_target_t *overlay_hit_target(const overlay_spec_t *s, int x, int y)
{
    if (!s) return NULL;
    for (int i = 0; i < s->n_targets; i++) {
        const overlay_target_t *t = &s->targets[i];
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h)
            return t;
    }
    return NULL;
}

overlay_atlas_t *overlay_find_atlas(overlay_spec_t *s, const char *id)
{
    if (!s || !id || !id[0]) return NULL;
    for (int i = 0; i < s->n_atlases; i++)
        if (strcmp(s->atlases[i].id, id) == 0) return &s->atlases[i];
    return NULL;
}

const overlay_glyph_t *overlay_glyph(const overlay_atlas_t *a, char ch)
{
    if (!a) return NULL;
    for (int i = 0; i < a->n_glyphs; i++)
        if (a->glyphs[i].ch == ch) return &a->glyphs[i];
    return NULL;
}

int overlay_text_width(const overlay_atlas_t *a, const char *text)
{
    if (!a || !text) return 0;
    int w = 0;
    for (const char *p = text; *p; p++) {
        const overlay_glyph_t *g = overlay_glyph(a, *p);
        w += g ? g->w : a->mean_w;
    }
    return w;
}

/* ---------- values document ---------- */

uint32_t overlay_values_apply(overlay_spec_t *s, const char *json, size_t len,
                              int32_t *last_seq)
{
    if (!s || !json || !len) return 0;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return 0;

    uint32_t changed = 0;
    do {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(root, "seq");
        if (!cJSON_IsNumber(seq)) break;
        int32_t sq = (int32_t)seq->valuedouble;
        /* Newest seq wins; equal seq = no repaint. */
        if (*last_seq >= 0 && sq <= *last_seq) break;

        const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
        if (!cJSON_IsObject(values)) break;
        *last_seq = sq;

        for (int i = 0; i < s->n_slots; i++) {
            overlay_slot_t *sl = &s->slots[i];
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, sl->key);
            if (!cJSON_IsString(v) || !v->valuestring) continue;
            char nv[OVERLAY_VALUE_CAP];
            /* Pre-formatted display string; firmware applies zero formatting
             * (over-long strings are truncated, then clipped at blit time). */
            snprintf(nv, sizeof nv, "%s", v->valuestring);
            if (strcmp(nv, sl->value) != 0) {
                strcpy(sl->value, nv);
                changed |= (1u << i);
            }
        }
    } while (0);

    cJSON_Delete(root);
    return changed;
}

/* ---------- framebuffer operations ---------- */

/* Pixel accessors for packed 4bpp (high nibble = even column, as the whole
 * codebase uses) and 1bpp (MSB = leftmost, 1 = white). */
static inline uint8_t px_get(const uint8_t *fb, int fb_w, int bpp, int x, int y)
{
    if (bpp == 4) {
        uint8_t b = fb[(size_t)y * (fb_w / 2) + x / 2];
        return (x & 1) ? (b & 0x0F) : (b >> 4);
    }
    uint8_t b = fb[(size_t)y * (fb_w / 8) + x / 8];
    return (uint8_t)((b >> (7 - (x & 7))) & 1);
}

static inline void px_set(uint8_t *fb, int fb_w, int bpp, int x, int y, uint8_t v)
{
    if (bpp == 4) {
        uint8_t *b = &fb[(size_t)y * (fb_w / 2) + x / 2];
        if (x & 1) *b = (uint8_t)((*b & 0xF0) | (v & 0x0F));
        else       *b = (uint8_t)((*b & 0x0F) | (uint8_t)(v << 4));
        return;
    }
    uint8_t *b = &fb[(size_t)y * (fb_w / 8) + x / 8];
    uint8_t bit = (uint8_t)(0x80 >> (x & 7));
    if (v) *b |= bit; else *b &= (uint8_t)~bit;
}

void overlay_invert_rect(uint8_t *fb, int fb_w, int fb_h, int bpp,
                         int x, int y, int w, int h)
{
    if (!fb || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > fb_w || y + h > fb_h) return;
    uint8_t maxv = (bpp == 4) ? 0xF : 1;
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            px_set(fb, fb_w, bpp, xx, yy,
                   (uint8_t)(maxv - px_get(fb, fb_w, bpp, xx, yy)));
}

/* Copy a rect from base into fb (slot background restore). */
static void restore_rect(uint8_t *fb, const uint8_t *base, int fb_w, int bpp,
                         int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            px_set(fb, fb_w, bpp, xx, yy, px_get(base, fb_w, bpp, xx, yy));
}

void overlay_draw_slot(uint8_t *fb, const uint8_t *base, int fb_w, int fb_h,
                       int bpp, const overlay_slot_t *slot,
                       const overlay_atlas_t *atlas)
{
    if (!fb || !base || !slot) return;
    (void)fb_h;   /* slot rects were clamped at parse time */

    restore_rect(fb, base, fb_w, bpp, slot->x, slot->y, slot->w, slot->h);
    if (!atlas || !atlas->bits || !slot->value[0]) return;

    int text_w = overlay_text_width(atlas, slot->value);
    int cx = slot->x;
    if (slot->align == OVERLAY_ALIGN_RIGHT)       cx = slot->x + slot->w - text_w;
    else if (slot->align == OVERLAY_ALIGN_CENTER) cx = slot->x + (slot->w - text_w) / 2;
    if (cx < slot->x) cx = slot->x;   /* over-long text: clip at the slot */

    int blit_h = atlas->height < slot->h ? atlas->height : slot->h;

    for (const char *p = slot->value; *p; p++) {
        const overlay_glyph_t *g = overlay_glyph(atlas, *p);
        int gw = g ? g->w : atlas->mean_w;   /* missing char: blank advance */
        if (g) {
            for (int gy = 0; gy < blit_h; gy++) {
                for (int gx = 0; gx < gw; gx++) {
                    int dx = cx + gx;
                    if (dx >= slot->x + slot->w) break;   /* clip right */
                    uint8_t v = px_get(atlas->bits, atlas->strip_w, atlas->bpp,
                                       g->x + gx, gy);
                    /* Atlas bpp matches the panel's on real specs; scale the
                     * 1bpp case onto 4bpp panels just in case (0 -> 0x0,
                     * 1 -> 0xF) so a mismatch degrades instead of breaking. */
                    if (atlas->bpp == 1 && bpp == 4) v = v ? 0xF : 0x0;
                    px_set(fb, fb_w, bpp, dx, slot->y + gy, v);
                }
            }
        }
        cx += gw;
        if (cx >= slot->x + slot->w) break;
    }
}
