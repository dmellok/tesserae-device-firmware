/* Pure frame-cache collection contract + album playback. */

#include "frame_collection.h"

#include <limits.h>
#include <string.h>

#include "cJSON.h"

static bool copy_required(char *dst, size_t cap, const cJSON *item)
{
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
        return false;
    size_t n = strlen(item->valuestring);
    if (n >= cap) return false;
    memcpy(dst, item->valuestring, n + 1);
    return true;
}
static bool copy_optional(char *dst, size_t cap, const cJSON *item)
{
    dst[0] = '\0';
    if (cJSON_IsNull(item) || item == NULL) return true;
    return copy_required(dst, cap, item);
}

static bool json_i32(const cJSON *item, int32_t min, int32_t max, int32_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    double d = item->valuedouble;
    if (d < (double)min || d > (double)max || d != (double)(int32_t)d)
        return false;
    *out = (int32_t)d;
    return true;
}

bool fc_digest_valid(const char *s)
{
    if (!s) return false;
    for (int i = 0; i < FC_DIGEST_HEX; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return s[FC_DIGEST_HEX] == '\0';
}

static bool parse_playback(const cJSON *root, fc_album_playback_t *out)
{
    const cJSON *producer = cJSON_GetObjectItemCaseSensitive(root, "producer");
    const cJSON *album = cJSON_IsObject(producer)
        ? cJSON_GetObjectItemCaseSensitive(producer, "album") : NULL;
    const cJSON *play = cJSON_IsObject(album)
        ? cJSON_GetObjectItemCaseSensitive(album, "playback") : NULL;
    if (!cJSON_IsObject(play)) return false;

    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(play, "mode");
    const cJSON *repeat = cJSON_GetObjectItemCaseSensitive(play, "repeat");
    int32_t interval = 0;
    if (!cJSON_IsString(mode) || !cJSON_IsString(repeat) ||
        !json_i32(cJSON_GetObjectItemCaseSensitive(play, "interval_s"),
                  1, INT32_MAX, &interval)) return false;

    if (strcmp(mode->valuestring, "sequential") == 0)
        out->mode = FC_MODE_SEQUENTIAL;
    else if (strcmp(mode->valuestring, "shuffle") == 0)
        out->mode = FC_MODE_SHUFFLE;
    else return false;

    if (strcmp(repeat->valuestring, "loop") == 0)
        out->repeat = FC_REPEAT_LOOP;
    else if (strcmp(repeat->valuestring, "reshuffle") == 0)
        out->repeat = FC_REPEAT_RESHUFFLE;
    else if (strcmp(repeat->valuestring, "once") == 0)
        out->repeat = FC_REPEAT_ONCE;
    else return false;

    out->interval_s = interval;
    return true;
}

static bool parse_cached_frame(const cJSON *jf, fc_frame_t *out)
{
    memset(out, 0, sizeof *out);
    if (!copy_required(out->frame_id, sizeof out->frame_id,
                       cJSON_GetObjectItemCaseSensitive(jf, "frame_id")) ||
        !copy_required(out->digest, sizeof out->digest,
                       cJSON_GetObjectItemCaseSensitive(jf, "digest")) ||
        !fc_digest_valid(out->digest) ||
        !copy_required(out->url, sizeof out->url,
                       cJSON_GetObjectItemCaseSensitive(jf, "url")) ||
        out->url[0] != '/') return false;

    int32_t bytes = 0;
    if (!json_i32(cJSON_GetObjectItemCaseSensitive(jf, "position"),
                  0, INT32_MAX, &out->position) ||
        !json_i32(cJSON_GetObjectItemCaseSensitive(jf, "bytes"),
                  1, INT32_MAX, &bytes) ||
        !json_i32(cJSON_GetObjectItemCaseSensitive(jf, "ttl_s"),
                  0, INT32_MAX, &out->ttl_s)) return false;
    out->bytes = (uint32_t)bytes;
    return true;
}

static bool duplicate_frame(const fc_manifest_t *m, const fc_frame_t *f)
{
    for (int i = 0; i < m->n_frames; i++) {
        const fc_frame_t *p = &m->frames[i];
        if (strcmp(p->frame_id, f->frame_id) == 0 ||
            p->position == f->position)
            return true;
    }
    return false;
}

static void sort_by_position(fc_manifest_t *m)
{
    for (int i = 1; i < m->n_frames; i++) {
        fc_frame_t cur = m->frames[i];
        int j = i;
        while (j > 0 && m->frames[j - 1].position > cur.position) {
            m->frames[j] = m->frames[j - 1];
            j--;
        }
        m->frames[j] = cur;
    }
}

bool fc_manifest_parse(const char *json, size_t len, fc_manifest_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!json || !len) return false;

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    bool ok = false;
    do {
        int32_t schema = 0, total = 0;
        if (!json_i32(cJSON_GetObjectItemCaseSensitive(root, "schema"),
                      1, INT32_MAX, &schema) || schema != 1) break;
        out->schema = (int)schema;
        if (!copy_required(out->collection_id, sizeof out->collection_id,
                           cJSON_GetObjectItemCaseSensitive(root, "collection_id")) ||
            !copy_required(out->kind, sizeof out->kind,
                           cJSON_GetObjectItemCaseSensitive(root, "kind")) ||
            strcmp(out->kind, "album") != 0 ||
            !copy_required(out->version, sizeof out->version,
                           cJSON_GetObjectItemCaseSensitive(root, "version")) ||
            !fc_digest_valid(out->version) ||
            !json_i32(cJSON_GetObjectItemCaseSensitive(root, "total_frames"),
                      1, INT32_MAX, &total) ||
            !copy_optional(out->cursor, sizeof out->cursor,
                           cJSON_GetObjectItemCaseSensitive(root, "cursor")) ||
            !copy_optional(out->next_cursor, sizeof out->next_cursor,
                           cJSON_GetObjectItemCaseSensitive(root, "next_cursor")) ||
            !parse_playback(root, &out->album)) break;
        out->total_frames = total;

        const cJSON *frames = cJSON_GetObjectItemCaseSensitive(root, "frames");
        if (!cJSON_IsArray(frames)) break;
        int listed = cJSON_GetArraySize(frames);
        if (listed <= 0 || total < listed) break;

        bool frames_ok = true;
        for (int i = 0; i < listed; i++) {
            const cJSON *jf = cJSON_GetArrayItem(frames, i);
            const cJSON *cache = cJSON_GetObjectItemCaseSensitive(jf, "cache");
            if (!cJSON_IsBool(cache)) { frames_ok = false; break; }
            if (cJSON_IsFalse(cache)) continue;
            if (out->n_frames >= FC_MAX_FRAMES) { frames_ok = false; break; }
            fc_frame_t f;
            if (!parse_cached_frame(jf, &f) || duplicate_frame(out, &f)) {
                frames_ok = false;
                break;
            }
            out->frames[out->n_frames++] = f;
        }
        if (!frames_ok || out->n_frames <= 0) break;
        sort_by_position(out);
        ok = true;
    } while (0);

    cJSON_Delete(root);
    if (!ok) memset(out, 0, sizeof *out);
    return ok;
}

const fc_frame_t *fc_find_frame_id(const fc_manifest_t *m, const char *frame_id)
{
    if (!m || !frame_id || !frame_id[0]) return NULL;
    for (int i = 0; i < m->n_frames; i++)
        if (strcmp(m->frames[i].frame_id, frame_id) == 0) return &m->frames[i];
    return NULL;
}

const fc_frame_t *fc_find_digest(const fc_manifest_t *m, const char *digest)
{
    if (!m || !digest || !digest[0]) return NULL;
    for (int i = 0; i < m->n_frames; i++)
        if (strcmp(m->frames[i].digest, digest) == 0) return &m->frames[i];
    return NULL;
}

static bool digest_in(const char list[][FC_DIGEST_HEX + 1], int n, const char *d)
{
    for (int i = 0; i < n; i++) if (strcmp(list[i], d) == 0) return true;
    return false;
}

int fc_sync_plan(const fc_manifest_t *m,
                 const char have[][FC_DIGEST_HEX + 1], int n_have,
                 char fetch[][FC_DIGEST_HEX + 1], int max_fetch,
                 char orphan[][FC_DIGEST_HEX + 1], int max_orphan,
                 int *n_orphan, bool *truncated)
{
    int nf = 0, no = 0;
    bool clipped = false;
    for (int i = 0; m && i < m->n_frames; i++) {
        const char *d = m->frames[i].digest;
        if (digest_in(have, n_have, d) ||
            digest_in((const char (*)[FC_DIGEST_HEX + 1])fetch, nf, d))
            continue;
        if (nf < max_fetch) strcpy(fetch[nf++], d);
        else clipped = true;
    }
    for (int i = 0; i < n_have; i++) {
        if (fc_find_digest(m, have[i])) continue;
        if (no < max_orphan) strcpy(orphan[no++], have[i]);
        else clipped = true;
    }
    if (n_orphan) *n_orphan = no;
    if (truncated) *truncated = clipped;
    return nf;
}

static uint32_t next_random(fc_play_state_t *s)
{
    uint32_t x = s->rng_state ? s->rng_state : 0x6d2b79f5u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng_state = x;
    return x;
}

static uint32_t frame_mask(int n)
{
    return n >= 32 ? UINT32_MAX : ((1u << n) - 1u);
}

void fc_play_reset(fc_play_state_t *s, const char *version, uint32_t seed)
{
    memset(s, 0, sizeof *s);
    s->current_index = -1;
    s->rng_state = seed ? seed : 0x6d2b79f5u;
    if (version) {
        strncpy(s->version, version, sizeof s->version - 1);
        s->version[sizeof s->version - 1] = '\0';
    }
}

static int choose_set_bit(uint32_t available, uint32_t pick)
{
    for (int i = 0; i < 32; i++) {
        if (!(available & (1u << i))) continue;
        if (pick == 0) return i;
        pick--;
    }
    return -1;
}

bool fc_play_next(const fc_manifest_t *m, fc_play_state_t *s, int *out_index)
{
    if (!m || !s || !out_index || m->n_frames <= 0) return false;
    if (strcmp(s->version, m->version) != 0) {
        uint32_t seed = s->rng_state;
        fc_play_reset(s, m->version, seed);
    }
    if (s->done) return false;

    int next = -1;
    if (m->album.mode == FC_MODE_SEQUENTIAL) {
        if (s->current_index < 0) next = 0;
        else if (s->current_index + 1 < m->n_frames) next = s->current_index + 1;
        else if (m->album.repeat == FC_REPEAT_ONCE) {
            s->done = true;
            return false;
        } else next = 0; /* sequential + loop/reshuffle both wrap in position order */
    } else {
        uint32_t all = frame_mask(m->n_frames);
        uint32_t available = all & ~s->used_mask;
        if (!available) {
            if (m->album.repeat == FC_REPEAT_ONCE) {
                s->done = true;
                return false;
            }
            /* A bitset cannot replay a previous random permutation. For shuffle,
             * loop is therefore a compatibility alias for a fresh bag. */
            s->used_mask = (m->n_frames > 1 && s->current_index >= 0)
                ? (1u << s->current_index) : 0;
            available = all & ~s->used_mask;
            if (!available) available = all; /* one-frame collection */
        }
        uint32_t count = 0;
        for (uint32_t bits = available; bits; bits &= bits - 1u) count++;
        next = choose_set_bit(available, next_random(s) % count);
        if (next < 0) return false;
        s->used_mask |= 1u << next;
    }

    s->current_index = (int16_t)next;
    *out_index = next;
    return true;
}

int32_t fc_interval_clamp(int32_t requested_s, int32_t min_s,
                          int32_t max_s, int32_t default_s)
{
    if (min_s <= 0 || max_s < min_s) return default_s;
    int32_t v = requested_s > 0 ? requested_s : default_s;
    if (v < min_s) return min_s;
    if (v > max_s) return max_s;
    return v;
}

int64_t fc_interrupt_resume_at(int64_t scheduled_at, int64_t now,
                               int32_t interval_s)
{
    /* An interruption must not slide an already scheduled Album deadline.
     * Otherwise recurring network frames can postpone local playback forever. */
    if (scheduled_at > 0) return scheduled_at;
    if (now <= 0 || interval_s <= 0 || now > INT64_MAX - interval_s) return 0;
    return now + interval_s;
}
