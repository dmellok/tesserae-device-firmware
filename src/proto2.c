/* proto2.c -- pure protocol-v2 logic. See proto2.h. Host-testable: no ESP-IDF. */

#include "proto2.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ---------- small helpers (mirror overlay.c) ---------- */

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
    for (int i = 0; i < P2_DIGEST_HEX; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[P2_DIGEST_HEX] == '\0';
}

/* Rect object {x,y,w,h}; clamp to panel. False = malformed or fully off. */
static bool parse_rect(const cJSON *o, int pw, int ph,
                       int *x, int *y, int *w, int *h)
{
    if (!cJSON_IsObject(o)) return false;
    const cJSON *jx = cJSON_GetObjectItemCaseSensitive(o, "x");
    const cJSON *jy = cJSON_GetObjectItemCaseSensitive(o, "y");
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(o, "w");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(o, "h");
    if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) ||
        !cJSON_IsNumber(jw) || !cJSON_IsNumber(jh)) return false;
    int rx = (int)jx->valuedouble, ry = (int)jy->valuedouble;
    int rw = (int)jw->valuedouble, rh = (int)jh->valuedouble;
    if (rw <= 0 || rh <= 0) return false;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > pw) rw = pw - rx;
    if (ry + rh > ph) rh = ph - ry;
    if (rw <= 0 || rh <= 0) return false;
    *x = rx; *y = ry; *w = rw; *h = rh;
    return true;
}

/* ---------- manifest parse ---------- */

/* Dedup an atlas by digest into m->atlases; returns slot index or -1. */
static int atlas_slot(p2_manifest_t *m, const cJSON *ja)
{
    if (!cJSON_IsObject(ja)) return -1;
    char digest[P2_DIGEST_HEX + 1];
    if (!copy_str(digest, sizeof digest,
                  cJSON_GetObjectItemCaseSensitive(ja, "digest")) ||
        !digest16_ok(digest))
        return -1;

    for (int i = 0; i < m->n_atlases; i++)
        if (strcmp(m->atlases[i].digest, digest) == 0) return i;
    if (m->n_atlases >= P2_MAX_ATLASES) return -1;

    p2_atlas_t *a = &m->atlases[m->n_atlases];
    memset(a, 0, sizeof *a);
    strcpy(a->digest, digest);
    if (!copy_str(a->url, sizeof a->url,
                  cJSON_GetObjectItemCaseSensitive(ja, "url")))
        return -1;
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(ja, "height");
    if (!cJSON_IsNumber(jh) || (int)jh->valuedouble <= 0 ||
        (int)jh->valuedouble > 128)
        return -1;
    a->height = (int)jh->valuedouble;

    const cJSON *glyphs = cJSON_GetObjectItemCaseSensitive(ja, "glyphs");
    if (!cJSON_IsObject(glyphs)) return -1;
    long wsum = 0;
    const cJSON *g;
    cJSON_ArrayForEach(g, glyphs) {
        if (a->n_glyphs >= P2_MAX_GLYPHS) return -1;   /* contract cap */
        if (!g->string || strlen(g->string) != 1) continue;
        const cJSON *gx = cJSON_GetObjectItemCaseSensitive(g, "x");
        const cJSON *gw = cJSON_GetObjectItemCaseSensitive(g, "w");
        if (!cJSON_IsNumber(gx) || !cJSON_IsNumber(gw)) continue;
        int vx = (int)gx->valuedouble, vw = (int)gw->valuedouble;
        if (vx < 0 || vw <= 0 || vx + vw > 4096) continue;
        p2_glyph_t *pg = &a->glyphs[a->n_glyphs++];
        pg->ch = g->string[0];
        pg->x = (uint16_t)vx;
        pg->w = (uint16_t)vw;
        if (vx + vw > a->strip_w) a->strip_w = vx + vw;
        wsum += vw;
    }
    if (a->n_glyphs == 0 || a->strip_w <= 0) return -1;
    a->mean_w = (int)(wsum / a->n_glyphs);
    if (a->mean_w < 1) a->mean_w = 1;
    return m->n_atlases++;
}

bool p2_manifest_parse(const char *json, size_t len,
                       int panel_w, int panel_h, p2_manifest_t *out)
{
    memset(out, 0, sizeof *out);
    if (!json || !len || panel_w <= 0 || panel_h <= 0) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ok = false;
    do {
        const cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "proto");
        if (!cJSON_IsNumber(proto) || (int)proto->valuedouble != P2_PROTO)
            break;
        if (!copy_str(out->frame_digest, sizeof out->frame_digest,
                      cJSON_GetObjectItemCaseSensitive(root, "frame_digest")) ||
            !digest16_ok(out->frame_digest))
            break;
        if (!copy_str(out->manifest_digest, sizeof out->manifest_digest,
                      cJSON_GetObjectItemCaseSensitive(root, "manifest_digest")) ||
            !digest16_ok(out->manifest_digest))
            break;

        const cJSON *regions = cJSON_GetObjectItemCaseSensitive(root, "regions");
        const cJSON *jr;
        if (cJSON_IsArray(regions)) cJSON_ArrayForEach(jr, regions) {
            if (out->n_regions >= P2_MAX_REGIONS) break;   /* first N win */
            p2_region_t *r = &out->regions[out->n_regions];
            memset(r, 0, sizeof *r);
            if (!copy_str(r->id, sizeof r->id,
                          cJSON_GetObjectItemCaseSensitive(jr, "id")))
                continue;
            if (!parse_rect(cJSON_GetObjectItemCaseSensitive(jr, "rect"),
                            panel_w, panel_h, &r->x, &r->y, &r->w, &r->h))
                continue;

            /* gestures: {"tap": true, "swipe": {...}, "slide": {"axis": ..}} */
            const cJSON *gs = cJSON_GetObjectItemCaseSensitive(jr, "gestures");
            if (cJSON_IsObject(gs)) {
                r->g_tap = cJSON_IsTrue(
                    cJSON_GetObjectItemCaseSensitive(gs, "tap"));
                const cJSON *sw = cJSON_GetObjectItemCaseSensitive(gs, "swipe");
                if (cJSON_IsObject(sw)) {
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sw, "left")))
                        r->g_swipe |= P2_SW_LEFT;
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sw, "right")))
                        r->g_swipe |= P2_SW_RIGHT;
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sw, "up")))
                        r->g_swipe |= P2_SW_UP;
                    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sw, "down")))
                        r->g_swipe |= P2_SW_DOWN;
                }
                const cJSON *sl = cJSON_GetObjectItemCaseSensitive(gs, "slide");
                if (cJSON_IsObject(sl)) {
                    char axis[4] = {0};
                    copy_str(axis, sizeof axis,
                             cJSON_GetObjectItemCaseSensitive(sl, "axis"));
                    if (axis[0] == 'x' || axis[0] == 'y') {
                        r->g_slide = true;
                        r->slide_axis = axis[0];
                    }
                }
            }
            if (!r->g_tap && !r->g_swipe && !r->g_slide)
                continue;   /* unreachable region */

            /* action: tier + type (+ nav target). Unknown type = downgrade. */
            bool downgrade = false;
            r->tier = 2;
            r->type = P2_ACT_UNKNOWN;
            const cJSON *act = cJSON_GetObjectItemCaseSensitive(jr, "action");
            if (cJSON_IsObject(act)) {
                const cJSON *jt = cJSON_GetObjectItemCaseSensitive(act, "tier");
                if (cJSON_IsNumber(jt)) {
                    int t = (int)jt->valuedouble;
                    if (t >= 0 && t <= 2) r->tier = t;
                }
                char ty[16] = {0};
                copy_str(ty, sizeof ty,
                         cJSON_GetObjectItemCaseSensitive(act, "type"));
                if      (strcmp(ty, "nav") == 0)          r->type = P2_ACT_NAV;
                else if (strcmp(ty, "ha") == 0)           r->type = P2_ACT_HA;
                else if (strcmp(ty, "webhook") == 0)      r->type = P2_ACT_WEBHOOK;
                else if (strcmp(ty, "refresh") == 0)      r->type = P2_ACT_REFRESH;
                else if (strcmp(ty, "fetch_latest") == 0) r->type = P2_ACT_FETCH_LATEST;
                else downgrade = true;                    /* unknown type */
                copy_str(r->target, sizeof r->target,
                         cJSON_GetObjectItemCaseSensitive(act, "target"));
            } else {
                downgrade = true;
            }

            /* feedback: mode (+ tiles / slider extras). Unknown = downgrade. */
            r->fb = P2_FB_INVERT;
            const cJSON *fb = cJSON_GetObjectItemCaseSensitive(jr, "feedback");
            if (cJSON_IsObject(fb)) {
                char mode[12] = {0};
                copy_str(mode, sizeof mode,
                         cJSON_GetObjectItemCaseSensitive(fb, "mode"));
                if      (strcmp(mode, "invert") == 0) r->fb = P2_FB_INVERT;
                else if (strcmp(mode, "none") == 0)   r->fb = P2_FB_NONE;
                else if (strcmp(mode, "tiles") == 0) {
                    r->fb = P2_FB_TILES;
                    copy_str(r->fb_set, sizeof r->fb_set,
                             cJSON_GetObjectItemCaseSensitive(fb, "set"));
                    const cJSON *cy = cJSON_GetObjectItemCaseSensitive(fb, "cycle");
                    const cJSON *cn;
                    if (cJSON_IsArray(cy)) cJSON_ArrayForEach(cn, cy) {
                        if (r->n_cycle >= P2_MAX_CYCLE) break;
                        if (copy_str(r->cycle[r->n_cycle],
                                     sizeof r->cycle[0], cn))
                            r->n_cycle++;
                    }
                    if (!r->fb_set[0] || r->n_cycle < 2) {
                        r->fb = P2_FB_INVERT;   /* unusable tile set */
                        downgrade = true;
                    }
                }
                else if (strcmp(mode, "slider") == 0) {
                    r->fb = P2_FB_SLIDER;
                    char tr[12] = {0};
                    copy_str(tr, sizeof tr,
                             cJSON_GetObjectItemCaseSensitive(fb, "track"));
                    r->fb_track = (tr[0] == 'h') ? 'h' : 'v';
                    copy_str(r->fb_value_text, sizeof r->fb_value_text,
                             cJSON_GetObjectItemCaseSensitive(fb, "value_text"));
                }
                else downgrade = true;         /* unknown feedback.mode */
            }
            if (downgrade) {
                if (r->fb == P2_FB_TILES || r->fb == P2_FB_SLIDER)
                    r->fb = P2_FB_INVERT;
                if (r->fb != P2_FB_NONE) r->fb = P2_FB_INVERT;
                r->tier = 2;
            }
            out->n_regions++;
        }

        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        const cJSON *jt;
        if (cJSON_IsArray(text)) cJSON_ArrayForEach(jt, text) {
            if (out->n_text >= P2_MAX_TEXT) break;
            p2_text_t *t = &out->text[out->n_text];
            memset(t, 0, sizeof *t);
            if (!copy_str(t->id, sizeof t->id,
                          cJSON_GetObjectItemCaseSensitive(jt, "id")))
                continue;
            if (!parse_rect(cJSON_GetObjectItemCaseSensitive(jt, "rect"),
                            panel_w, panel_h, &t->x, &t->y, &t->w, &t->h))
                continue;
            if (!copy_str(t->key, sizeof t->key,
                          cJSON_GetObjectItemCaseSensitive(jt, "key")))
                continue;
            char al[8] = {0};
            copy_str(al, sizeof al, cJSON_GetObjectItemCaseSensitive(jt, "align"));
            t->align = (al[0] == 'c') ? 1 : (al[0] == 'r') ? 2 : 0;
            const cJSON *mc = cJSON_GetObjectItemCaseSensitive(jt, "max_chars");
            t->max_chars = cJSON_IsNumber(mc) ? (int)mc->valuedouble
                                              : P2_VALUE_CAP - 1;
            if (t->max_chars < 1) continue;
            if (t->max_chars > P2_VALUE_CAP - 1) t->max_chars = P2_VALUE_CAP - 1;
            t->atlas_idx = atlas_slot(out,
                cJSON_GetObjectItemCaseSensitive(jt, "atlas"));
            if (t->atlas_idx < 0) continue;    /* unrenderable without atlas */
            out->n_text++;
        }

        ok = true;
    } while (0);

    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

bool p2_manifest_matches(const p2_manifest_t *m, const char *d)
{
    return m && d && d[0] && strncmp(m->frame_digest, d, P2_DIGEST_HEX) == 0;
}

/* ---------- gestures ---------- */

p2_gesture_t p2_classify(int x0, int y0, int x1, int y1, uint32_t ms)
{
    (void)ms;   /* see header: long-press deliberately classifies as tap */
    int dx = x1 - x0, dy = y1 - y0;
    long d2 = (long)dx * dx + (long)dy * dy;
    if (d2 >= (long)P2_SWIPE_MIN_PX * P2_SWIPE_MIN_PX) {
        if (dx * dx >= dy * dy)
            return dx < 0 ? P2_G_SWIPE_LEFT : P2_G_SWIPE_RIGHT;
        return dy < 0 ? P2_G_SWIPE_UP : P2_G_SWIPE_DOWN;
    }
    if (d2 < (long)P2_TAP_MAX_PX * P2_TAP_MAX_PX)
        return P2_G_TAP;
    return P2_G_NONE;   /* 20-40 px dead zone: ambiguous, ignored */
}

static bool rect_contains(const p2_region_t *r, int px, int py)
{
    return px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h;
}

static uint8_t swipe_bit(p2_gesture_t g)
{
    switch (g) {
    case P2_G_SWIPE_LEFT:  return P2_SW_LEFT;
    case P2_G_SWIPE_RIGHT: return P2_SW_RIGHT;
    case P2_G_SWIPE_UP:    return P2_SW_UP;
    case P2_G_SWIPE_DOWN:  return P2_SW_DOWN;
    default:               return 0;
    }
}

static int slide_value(const p2_region_t *r, int lx, int ly)
{
    long v;
    if (r->slide_axis == 'y')
        v = ((long)(r->y + r->h - ly) * 100) / r->h;   /* bottom=0, top=100 */
    else
        v = ((long)(lx - r->x) * 100) / r->w;          /* left=0, right=100 */
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return (int)v;
}

int p2_hit(const p2_manifest_t *m, int x0, int y0, int x1, int y1,
           uint32_t ms, p2_gesture_t *out_gesture, int *out_value)
{
    if (!m || !out_gesture) return -1;
    p2_gesture_t g = p2_classify(x0, y0, x1, y1, ms);
    *out_gesture = g;

    /* Pass 1: down point, document order, first hit wins. A slide region
     * absorbs EVERY gesture landing in it. */
    for (int i = 0; i < m->n_regions; i++) {
        const p2_region_t *r = &m->regions[i];
        if (!rect_contains(r, x0, y0)) continue;
        if (r->g_slide) {
            *out_gesture = P2_G_SLIDE;
            if (out_value) *out_value = slide_value(r, x1, y1);
            return i;
        }
        if (g == P2_G_TAP && r->g_tap) return i;
        if ((swipe_bit(g) & r->g_swipe) != 0) return i;
    }

    /* Pass 2: a swipe that started just outside a small zone -- retry with
     * the lift point (swipe-declaring regions only). */
    uint8_t sb = swipe_bit(g);
    if (sb) {
        for (int i = 0; i < m->n_regions; i++) {
            const p2_region_t *r = &m->regions[i];
            if ((r->g_swipe & sb) && rect_contains(r, x1, y1)) return i;
        }
    }
    return -1;
}

/* ---------- state bundles ---------- */

bool p2_bundle_parse(const char *json, size_t len, uint32_t frame_bytes,
                     p2_bundle_t *out)
{
    memset(out, 0, sizeof *out);
    if (!json || !len) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ok = false;
    do {
        if (!copy_str(out->bundle_digest, sizeof out->bundle_digest,
                      cJSON_GetObjectItemCaseSensitive(root, "bundle_digest")) ||
            !digest16_ok(out->bundle_digest))
            break;

        const cJSON *states = cJSON_GetObjectItemCaseSensitive(root, "states");
        const cJSON *js;
        if (cJSON_IsArray(states)) cJSON_ArrayForEach(js, states) {
            if (out->n_states >= P2_MAX_BSTATES) break;
            p2_bstate_t *st = &out->states[out->n_states];
            memset(st, 0, sizeof *st);
            char kind[8] = {0};
            copy_str(kind, sizeof kind,
                     cJSON_GetObjectItemCaseSensitive(js, "kind"));
            if (!copy_str(st->state_id, sizeof st->state_id,
                          cJSON_GetObjectItemCaseSensitive(js, "state_id")) ||
                !copy_str(st->url, sizeof st->url,
                          cJSON_GetObjectItemCaseSensitive(js, "url")))
                continue;
            const cJSON *jb = cJSON_GetObjectItemCaseSensitive(js, "bytes");
            if (!cJSON_IsNumber(jb) || jb->valuedouble < 1) continue;
            st->bytes = (uint32_t)jb->valuedouble;

            if (strcmp(kind, "frame") == 0) {
                st->kind = P2_BK_FRAME;
                if (!copy_str(st->digest, sizeof st->digest,
                              cJSON_GetObjectItemCaseSensitive(js, "frame_digest")) ||
                    !digest16_ok(st->digest))
                    continue;
                copy_str(st->man_digest, sizeof st->man_digest,
                         cJSON_GetObjectItemCaseSensitive(js, "manifest_digest"));
                const cJSON *jt = cJSON_GetObjectItemCaseSensitive(js, "ttl_s");
                st->ttl_s = cJSON_IsNumber(jt) ? (int32_t)jt->valuedouble : 0;
                if (frame_bytes && st->bytes != frame_bytes) continue;
            } else if (strcmp(kind, "tile") == 0) {
                st->kind = P2_BK_TILE;
                if (!copy_str(st->digest, sizeof st->digest,
                              cJSON_GetObjectItemCaseSensitive(js, "tile_digest")) ||
                    !digest16_ok(st->digest))
                    continue;
                const cJSON *jr = cJSON_GetObjectItemCaseSensitive(js, "rect");
                if (!cJSON_IsObject(jr)) continue;
                const cJSON *jx = cJSON_GetObjectItemCaseSensitive(jr, "x");
                const cJSON *jy = cJSON_GetObjectItemCaseSensitive(jr, "y");
                const cJSON *jw = cJSON_GetObjectItemCaseSensitive(jr, "w");
                const cJSON *jh = cJSON_GetObjectItemCaseSensitive(jr, "h");
                if (!cJSON_IsNumber(jx) || !cJSON_IsNumber(jy) ||
                    !cJSON_IsNumber(jw) || !cJSON_IsNumber(jh)) continue;
                st->x = (int)jx->valuedouble; st->y = (int)jy->valuedouble;
                st->w = (int)jw->valuedouble; st->h = (int)jh->valuedouble;
                if (st->w <= 0 || st->h <= 0 ||
                    (st->x & 1) || (st->w & 1)) continue;  /* wire even rule */
                if (st->bytes != (uint32_t)(st->w / 2) * (uint32_t)st->h)
                    continue;                              /* size mismatch */
            } else {
                continue;   /* unknown kind: ignore (additive) */
            }
            out->n_states++;
        }

        const cJSON *links = cJSON_GetObjectItemCaseSensitive(root, "links");
        const cJSON *jf;
        if (cJSON_IsObject(links)) cJSON_ArrayForEach(jf, links) {
            if (!jf->string || !cJSON_IsObject(jf)) continue;
            const cJSON *jk;
            cJSON_ArrayForEach(jk, jf) {
                if (out->n_links >= P2_MAX_LINKS) break;
                if (!jk->string || !cJSON_IsString(jk) || !jk->valuestring)
                    continue;
                p2_blink_t *l = &out->links[out->n_links];
                if (strlen(jf->string) >= sizeof l->from ||
                    strlen(jk->string) >= sizeof l->key ||
                    strlen(jk->valuestring) >= sizeof l->to) continue;
                strcpy(l->from, jf->string);
                strcpy(l->key, jk->string);
                strcpy(l->to, jk->valuestring);
                out->n_links++;
            }
        }
        ok = true;
    } while (0);

    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

const p2_bstate_t *p2_bundle_state(const p2_bundle_t *b, const char *state_id)
{
    if (!b || !state_id) return NULL;
    for (int i = 0; i < b->n_states; i++)
        if (strcmp(b->states[i].state_id, state_id) == 0) return &b->states[i];
    return NULL;
}

const char *p2_bundle_link(const p2_bundle_t *b, const char *from,
                           const char *key)
{
    if (!b || !from || !key) return NULL;
    for (int i = 0; i < b->n_links; i++)
        if (strcmp(b->links[i].from, from) == 0 &&
            strcmp(b->links[i].key, key) == 0)
            return b->links[i].to;
    return NULL;
}

const char *p2_gesture_name(p2_gesture_t g)
{
    switch (g) {
    case P2_G_TAP:         return "tap";
    case P2_G_SWIPE_LEFT:  return "swipe_left";
    case P2_G_SWIPE_RIGHT: return "swipe_right";
    case P2_G_SWIPE_UP:    return "swipe_up";
    case P2_G_SWIPE_DOWN:  return "swipe_down";
    case P2_G_SLIDE:       return "slide";
    default:               return "none";
    }
}

/* ---------- text rendering (4bpp wire format) ---------- */

const p2_glyph_t *p2_glyph(const p2_atlas_t *a, char ch)
{
    if (!a) return NULL;
    for (int i = 0; i < a->n_glyphs; i++)
        if (a->glyphs[i].ch == ch) return &a->glyphs[i];
    return NULL;
}

int p2_text_width(const p2_atlas_t *a, const char *text)
{
    if (!a || !text) return 0;
    int w = 0;
    for (const char *p = text; *p; p++) {
        const p2_glyph_t *g = p2_glyph(a, *p);
        w += g ? g->w : a->mean_w;
    }
    return w;
}

/* 4bpp nibble accessors (2 px/byte, HIGH nibble = left pixel): the atlas
 * strips are always this shape on the wire. */
static inline uint8_t px_get(const uint8_t *buf, int stride, int x, int y)
{
    uint8_t b = buf[(size_t)y * stride + (x >> 1)];
    return (x & 1) ? (b & 0x0F) : (b >> 4);
}

/* Framebuffer write at the panel's depth: 4bpp nibbles, 2bpp pairs (MSB pair
 * = leftmost, 0 black .. 3 white) or 1bpp (MSB = leftmost, 1 = white). The
 * level arrives on the atlas's 4bpp scale and is folded to the panel's. */
static inline void px_put(uint8_t *fb, int fb_w, int bpp, int x, int y, uint8_t v4)
{
    if (bpp == 4) {
        uint8_t *b = &fb[(size_t)y * (fb_w / 2) + (x >> 1)];
        if (x & 1) *b = (uint8_t)((*b & 0xF0) | (v4 & 0x0F));
        else       *b = (uint8_t)((*b & 0x0F) | (v4 << 4));
    } else if (bpp == 2) {
        uint8_t *b = &fb[(size_t)y * ((fb_w + 3) / 4) + (x >> 2)];
        int sh = (3 - (x & 3)) * 2;
        *b = (uint8_t)((*b & ~(0x3 << sh)) | (((v4 >> 2) & 0x3) << sh));
    } else {
        uint8_t *b = &fb[(size_t)y * ((fb_w + 7) / 8) + (x >> 3)];
        uint8_t m = (uint8_t)(0x80 >> (x & 7));
        if (v4 >= 8) *b |= m; else *b = (uint8_t)(*b & ~m);
    }
}

void p2_draw_text(uint8_t *fb, int fb_w, int fb_h, int bpp,
                  const p2_text_t *t, const p2_atlas_t *a, const char *str)
{
    if (!fb || !t || !a || !a->bits || !str) return;
    if (bpp != 4 && bpp != 2 && bpp != 1) return;
    if (t->x < 0 || t->y < 0 || t->x + t->w > fb_w || t->y + t->h > fb_h)
        return;

    char clipped[P2_VALUE_CAP];
    snprintf(clipped, sizeof clipped, "%.*s", t->max_chars, str);

    const int st_stride = (a->strip_w + 1) / 2;

    /* Clear the region to white (every depth packs white as all-ones bytes).
     * Text rects follow the wire even-x/even-w rule, so on a 4bpp panel the
     * rows are whole bytes; deeper packings and a server that ever violates
     * the rule fall back to per-pixel writes. */
    const int ppb = 8 / bpp;
    if ((t->x % ppb) == 0 && (t->w % ppb) == 0) {
        const int fb_stride = fb_w / ppb;
        for (int r = 0; r < t->h; r++)
            memset(fb + (size_t)(t->y + r) * fb_stride + t->x / ppb, 0xFF, t->w / ppb);
    } else {
        for (int r = 0; r < t->h; r++)
            for (int c = 0; c < t->w; c++)
                px_put(fb, fb_w, bpp, t->x + c, t->y + r, 0xF);
    }

    int total = p2_text_width(a, clipped);
    int pen = t->x;
    if (t->align == 1)      pen = t->x + (t->w - total) / 2;
    else if (t->align == 2) pen = t->x + t->w - total;
    if (pen < t->x) pen = t->x;

    int band = a->height < t->h ? a->height : t->h;
    int ty0 = t->y + (t->h - band) / 2;

    for (const char *p = clipped; *p; p++) {
        const p2_glyph_t *g = p2_glyph(a, *p);
        if (!g) { pen += a->mean_w; continue; }   /* stable blank */
        for (int gx = 0; gx < g->w; gx++) {
            int dx = pen + gx;
            if (dx >= t->x + t->w) break;         /* clip right edge */
            for (int gy = 0; gy < band; gy++)
                px_put(fb, fb_w, bpp, dx, ty0 + gy,
                       px_get(a->bits, st_stride, g->x + gx, gy));
        }
        pen += g->w;
        if (pen >= t->x + t->w) break;
    }
}
