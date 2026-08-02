/*
 * frame_collection.h: pure frame-cache collection contract + album playback.
 *
 * This is the firmware half of docs/dev/frame-cache.md in the Tesserae server.
 * It has no ESP-IDF dependencies so the exact server JSON, cache diff, interval
 * policy and shuffle bag run in host tests before they touch SD or the panel.
 *
 * Slice 1 deliberately retains at most 32 cache:true frames. The server may
 * describe a larger total and mark overflow entries cache:false; those entries
 * count toward total_frames but are not retained for offline playback.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FC_MAX_FRAMES       32
#define FC_ID_CAP           64
#define FC_KIND_CAP         16
#define FC_FRAME_ID_CAP     64
#define FC_VERSION_CAP      32
#define FC_DIGEST_HEX       16
#define FC_URL_CAP          192
#define FC_CURSOR_CAP       96

typedef enum {
    FC_MODE_SEQUENTIAL = 0,
    FC_MODE_SHUFFLE,
} fc_play_mode_t;

typedef enum {
    FC_REPEAT_LOOP = 0,
    FC_REPEAT_RESHUFFLE,
    FC_REPEAT_ONCE,
} fc_repeat_t;

typedef struct {
    char     frame_id[FC_FRAME_ID_CAP];
    int32_t  position;
    char     digest[FC_DIGEST_HEX + 1];
    uint32_t bytes;
    int32_t  ttl_s;
    char     url[FC_URL_CAP];
} fc_frame_t;

typedef struct {
    fc_play_mode_t mode;
    fc_repeat_t    repeat;
    int32_t        interval_s;   /* server request; board code clamps it */
} fc_album_playback_t;

typedef struct {
    int      schema;
    char     collection_id[FC_ID_CAP];
    char     kind[FC_KIND_CAP];
    char     version[FC_VERSION_CAP];
    int32_t  total_frames;       /* includes cache:false entries */
    char     cursor[FC_CURSOR_CAP];
    char     next_cursor[FC_CURSOR_CAP];
    int      n_frames;           /* retained cache:true entries, <= 32 */
    fc_frame_t frames[FC_MAX_FRAMES];
    fc_album_playback_t album;
} fc_manifest_t;

/* Parse schema-1 kind:album collection JSON. Strict for every cache:true frame;
 * cache:false entries are counted but not retained. Unknown additive fields are
 * ignored. More than FC_MAX_FRAMES cache:true entries fails the whole parse.
 * Retained frames are sorted by their producer-supplied position. */
bool fc_manifest_parse(const char *json, size_t len, fc_manifest_t *out);

/* Lookup retained frames. */
const fc_frame_t *fc_find_frame_id(const fc_manifest_t *m, const char *frame_id);
const fc_frame_t *fc_find_digest(const fc_manifest_t *m, const char *digest);

/* Cache differ over content digests. Emits missing manifest digests and cached
 * digests no longer referenced, truncating each output to its supplied cap. */
int fc_sync_plan(const fc_manifest_t *m,
                 const char have[][FC_DIGEST_HEX + 1], int n_have,
                 char fetch[][FC_DIGEST_HEX + 1], int max_fetch,
                 char orphan[][FC_DIGEST_HEX + 1], int max_orphan,
                 int *n_orphan);

bool fc_digest_valid(const char *s);

/* Pure album traversal state. Keep this in RTC RAM on-device: it changes every
 * frame and must not cause an NVS write. Reset whenever manifest.version moves. */
typedef struct {
    char     version[FC_VERSION_CAP];
    int16_t  current_index;      /* -1 before the first frame */
    uint32_t used_mask;          /* shuffle bag, one bit per retained frame */
    uint32_t rng_state;
    bool     done;               /* repeat:once exhausted */
} fc_play_state_t;

void fc_play_reset(fc_play_state_t *s, const char *version, uint32_t seed);

/* Select the next retained frame index to paint. False means repeat:once has
 * completed (the current frame remains on glass). For shuffle, both loop and
 * reshuffle start a fresh bag; the previous frame is excluded from the first
 * draw of a new bag when at least two frames exist. */
bool fc_play_next(const fc_manifest_t *m, fc_play_state_t *s, int *out_index);

/* Clamp a producer-requested interval. Non-positive requests use default_s;
 * malformed caller bounds also fall back to default_s. */
int32_t fc_interval_clamp(int32_t requested_s, int32_t min_s,
                          int32_t max_s, int32_t default_s);
