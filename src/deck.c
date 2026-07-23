/* deck.c -- pure deck-cache logic. See deck.h. Host-testable: no ESP-IDF. */

#include "deck.h"

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

bool deck_digest_valid(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < DECK_DIGEST_HEX; i++) {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return s[DECK_DIGEST_HEX] == '\0';
}

/* ---------- manifest parse ---------- */

/* Parse one link object. Returns false to DROP the link (bad shape), which is
 * deliberately non-fatal for the manifest as a whole. Target existence is
 * checked by the caller after all pages are known. */
static bool parse_link(const cJSON *jl, deck_link_t *out)
{
    memset(out, 0, sizeof *out);

    const cJSON *btn  = cJSON_GetObjectItemCaseSensitive(jl, "button");
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(jl, "zone");
    const cJSON *tgt  = cJSON_GetObjectItemCaseSensitive(jl, "target_page_id");

    if (!copy_str(out->target_page_id, sizeof out->target_page_id, tgt))
        return false;

    bool have_button = cJSON_IsString(btn) && btn->valuestring[0];
    bool have_zone   = cJSON_IsObject(zone);
    if (!have_button && !have_zone) return false;   /* dead link */

    if (have_button) {
        if (strlen(btn->valuestring) >= sizeof out->button) return false;
        strcpy(out->button, btn->valuestring);
    }
    if (have_zone) {
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(zone, "x");
        const cJSON *y = cJSON_GetObjectItemCaseSensitive(zone, "y");
        const cJSON *w = cJSON_GetObjectItemCaseSensitive(zone, "w");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(zone, "h");
        if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) ||
            !cJSON_IsNumber(w) || !cJSON_IsNumber(h)) return false;
        out->zx = (float)x->valuedouble;
        out->zy = (float)y->valuedouble;
        out->zw = (float)w->valuedouble;
        out->zh = (float)h->valuedouble;
        /* Normalised rect sanity: inside the unit square, positive area. */
        if (out->zx < 0.f || out->zy < 0.f || out->zw <= 0.f || out->zh <= 0.f ||
            out->zx + out->zw > 1.0001f || out->zy + out->zh > 1.0001f)
            return false;
        out->has_zone = true;
    }
    return true;
}

bool deck_manifest_parse(const char *json, size_t len, deck_manifest_t *out)
{
    memset(out, 0, sizeof *out);
    if (!json || !len) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ok = false;
    do {
        if (!copy_str(out->deck_id, sizeof out->deck_id,
                      cJSON_GetObjectItemCaseSensitive(root, "deck_id"))) break;
        if (!copy_str(out->version, sizeof out->version,
                      cJSON_GetObjectItemCaseSensitive(root, "version"))) break;
        if (!copy_str(out->entry_page_id, sizeof out->entry_page_id,
                      cJSON_GetObjectItemCaseSensitive(root, "entry_page_id"))) break;

        const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
        if (!cJSON_IsArray(pages)) break;
        int n = cJSON_GetArraySize(pages);
        if (n <= 0 || n > DECK_MAX_PAGES) break;

        bool pages_ok = true;
        for (int i = 0; i < n && pages_ok; i++) {
            const cJSON *jp = cJSON_GetArrayItem(pages, i);
            deck_page_t *p = &out->pages[i];

            if (!copy_str(p->page_id, sizeof p->page_id,
                          cJSON_GetObjectItemCaseSensitive(jp, "page_id")) ||
                !copy_str(p->digest, sizeof p->digest,
                          cJSON_GetObjectItemCaseSensitive(jp, "digest")) ||
                !deck_digest_valid(p->digest)) {
                pages_ok = false;
                break;
            }
            const cJSON *jb = cJSON_GetObjectItemCaseSensitive(jp, "bytes");
            const cJSON *jt = cJSON_GetObjectItemCaseSensitive(jp, "ttl_s");
            if (!cJSON_IsNumber(jb) || jb->valuedouble < 1 ||
                jb->valuedouble > 0xFFFFFFFF ||
                !cJSON_IsNumber(jt) || jt->valuedouble < 0) {
                pages_ok = false;
                break;
            }
            p->bytes = (uint32_t)jb->valuedouble;
            p->ttl_s = (int32_t)jt->valuedouble;

            const cJSON *links = cJSON_GetObjectItemCaseSensitive(jp, "links");
            if (links && !cJSON_IsArray(links)) { pages_ok = false; break; }
            int nl = links ? cJSON_GetArraySize(links) : 0;
            for (int j = 0; j < nl && p->n_links < DECK_MAX_LINKS; j++) {
                deck_link_t l;
                if (parse_link(cJSON_GetArrayItem(links, j), &l))
                    p->links[p->n_links++] = l;
                /* else: drop the malformed link, keep the deck */
            }
        }
        if (!pages_ok) break;
        out->n_pages = n;

        /* Entry page must exist. */
        if (!deck_find_page(out, out->entry_page_id)) break;

        /* Drop links whose target page does not exist. */
        for (int i = 0; i < out->n_pages; i++) {
            deck_page_t *p = &out->pages[i];
            int kept = 0;
            for (int j = 0; j < p->n_links; j++) {
                if (deck_find_page(out, p->links[j].target_page_id))
                    p->links[kept++] = p->links[j];
            }
            p->n_links = kept;
        }
        ok = true;
    } while (0);

    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

/* ---------- lookup + hit-testing ---------- */

const deck_page_t *deck_find_page(const deck_manifest_t *m, const char *page_id)
{
    if (!m || !page_id || !page_id[0]) return NULL;
    for (int i = 0; i < m->n_pages; i++)
        if (strcmp(m->pages[i].page_id, page_id) == 0) return &m->pages[i];
    return NULL;
}

const char *deck_nav_button(const deck_manifest_t *m, const char *page_id,
                            const char *button)
{
    const deck_page_t *p = deck_find_page(m, page_id);
    if (!p || !button || !button[0]) return NULL;
    for (int i = 0; i < p->n_links; i++)
        if (p->links[i].button[0] && strcmp(p->links[i].button, button) == 0)
            return p->links[i].target_page_id;
    return NULL;
}

const char *deck_nav_touch(const deck_manifest_t *m, const char *page_id,
                           int x, int y, int panel_w, int panel_h)
{
    const deck_page_t *p = deck_find_page(m, page_id);
    if (!p || panel_w <= 0 || panel_h <= 0) return NULL;
    if (x < 0 || y < 0 || x >= panel_w || y >= panel_h) return NULL;

    /* Pixel-centre rule: left/top edges inclusive, right/bottom exclusive, so
     * abutting zones never both claim a boundary pixel. See deck.h. */
    float px = ((float)x + 0.5f) / (float)panel_w;
    float py = ((float)y + 0.5f) / (float)panel_h;

    for (int i = 0; i < p->n_links; i++) {
        const deck_link_t *l = &p->links[i];
        if (!l->has_zone) continue;
        if (px >= l->zx && px < l->zx + l->zw &&
            py >= l->zy && py < l->zy + l->zh)
            return l->target_page_id;
    }
    return NULL;
}

/* ---------- sync differ ---------- */

static bool in_list(const char list[][DECK_DIGEST_HEX + 1], int n, const char *d)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], d) == 0) return true;
    return false;
}

int deck_sync_plan(const deck_manifest_t *m,
                   const char have[][DECK_DIGEST_HEX + 1], int n_have,
                   char fetch[][DECK_DIGEST_HEX + 1], int max_fetch,
                   char orphan[][DECK_DIGEST_HEX + 1], int max_orphan,
                   int *n_orphan)
{
    int nf = 0, no = 0;

    /* Missing: referenced but not on card (dedup across pages). */
    for (int i = 0; m && i < m->n_pages; i++) {
        const char *d = m->pages[i].digest;
        if (in_list(have, n_have, d)) continue;
        if (nf < max_fetch && !in_list((const char (*)[DECK_DIGEST_HEX + 1])fetch, nf, d)) {
            strcpy(fetch[nf++], d);
        }
    }

    /* Orphans: on card but referenced by no page. */
    for (int i = 0; i < n_have; i++) {
        bool referenced = false;
        for (int j = 0; m && j < m->n_pages; j++) {
            if (strcmp(m->pages[j].digest, have[i]) == 0) { referenced = true; break; }
        }
        if (!referenced && no < max_orphan) strcpy(orphan[no++], have[i]);
    }

    if (n_orphan) *n_orphan = no;
    return nf;
}

/* ---------- digest rules ---------- */

void deck_digest_hex16(const uint8_t sha[32], char out[DECK_DIGEST_HEX + 1])
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < DECK_DIGEST_HEX / 2; i++) {
        out[i * 2]     = hexd[sha[i] >> 4];
        out[i * 2 + 1] = hexd[sha[i] & 0xF];
    }
    out[DECK_DIGEST_HEX] = '\0';
}

bool deck_digest_check(const uint8_t *frame, size_t len,
                       uint32_t expect_bytes, const char *expect_digest)
{
    if (!frame || !len) return false;
    if (len != (size_t)expect_bytes) return false;
    if (!deck_digest_valid(expect_digest)) return false;

    uint8_t sha[32];
    deck_sha256(frame, len, sha);
    char hex[DECK_DIGEST_HEX + 1];
    deck_digest_hex16(sha, hex);
    return strcmp(hex, expect_digest) == 0;
}
