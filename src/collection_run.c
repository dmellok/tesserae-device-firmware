/* collection_run.c -- offline Album orchestration. See header. */

#include "collection_run.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"

#include "app_config.h"
#include "collection_cache.h"
#include "epd_driver.h"
#include "frame_collection.h"
#include "image_fetcher.h"
#include "net_rest.h"
#include "rest_config.h"
#include "sdcard.h"

static const char *TAG = "collection";

#define COLLECTION_MANIFEST_BUF (32 * 1024 + 1)
#define SCHEDULE_MAGIC          0x434f4c31u  /* "COL1" */
#define EPOCH_MIN               1600000000LL
#define EPOCH_MAX               4102444800LL

/* Frame traversal and deadlines change frequently, so keep them out of NVS. */
RTC_DATA_ATTR static uint32_t        s_schedule_magic;
RTC_DATA_ATTR static fc_play_state_t s_play;
RTC_DATA_ATTR static int64_t         s_next_frame_at;
RTC_DATA_ATTR static int64_t         s_next_network_at;

static fc_manifest_t *s_manifest;
static bool           s_have_collection;

static bool wall_clock_now(int64_t *out)
{
    time_t now = time(NULL);
    if ((int64_t)now < EPOCH_MIN || (int64_t)now > EPOCH_MAX) return false;
    *out = (int64_t)now;
    return true;
}

static int32_t album_interval(void)
{
    if (!s_have_collection || !s_manifest) return SLEEP_INTERVAL_S;
    return fc_interval_clamp(s_manifest->album.interval_s,
                             SLEEP_INTERVAL_MIN_S, SLEEP_INTERVAL_MAX_S,
                             SLEEP_INTERVAL_S);
}

static void schedule_reset(void)
{
    memset(&s_play, 0, sizeof s_play);
    s_play.current_index = -1;
    s_next_frame_at = 0;
    s_next_network_at = 0;
    s_schedule_magic = SCHEDULE_MAGIC;
}

static void report(const char *state)
{
    const rest_config_t *c = rest_config_get();
    if (!c->collection_id[0] || !c->collection_srv_ver[0]) {
        rest_set_collection_report(NULL, NULL, 0, 0, NULL);
        return;
    }
    int cached = 0;
    uint32_t total = 0;
    if (s_have_collection && s_manifest) {
        cached = s_manifest->n_frames;
        total = (uint32_t)s_manifest->total_frames;
    } else if (sdcard_mounted()) {
        char have[FC_MAX_FRAMES][FC_DIGEST_HEX + 1];
        cached = collection_cache_list(c->collection_id, have, FC_MAX_FRAMES);
    }
    rest_set_collection_report(c->collection_id, c->collection_srv_ver,
                               (uint16_t)cached, total, state);
}

void collection_boot(void)
{
    if (!sdcard_mounted() && !sdcard_mount()) return;
    rest_set_frame_cache_capability(sdcard_free_bytes(), FC_MAX_FRAMES);
    if (s_schedule_magic != SCHEDULE_MAGIC) schedule_reset();

    const rest_config_t *c = rest_config_get();
    if (!c->collection_id[0] || !c->collection_synced_ver[0] ||
        strcmp(c->collection_synced_ver, c->collection_srv_ver) != 0) {
        if (c->collection_id[0]) report("syncing");
        return;
    }

    s_manifest = malloc(sizeof *s_manifest);
    if (!s_manifest) { report("error"); return; }
    if (!collection_cache_load_manifest(c->collection_id, s_manifest) ||
        strcmp(s_manifest->collection_id, c->collection_id) != 0 ||
        strcmp(s_manifest->version, c->collection_synced_ver) != 0) {
        free(s_manifest);
        s_manifest = NULL;
        report("error");
        return;
    }
    s_have_collection = true;
    if (strcmp(s_play.version, s_manifest->version) != 0)
        fc_play_reset(&s_play, s_manifest->version, esp_random());
    report(s_play.done ? "paused" : "playing");
    ESP_LOGI(TAG, "offline Album '%s' v%s ready (%d/%ld cached)",
             s_manifest->collection_id, s_manifest->version,
             s_manifest->n_frames, (long)s_manifest->total_frames);
}

static void force_resync(const char *reason)
{
    ESP_LOGW(TAG, "%s; forcing collection resync", reason);
    rest_config_set_collection_synced_ver("");
    rest_config_save();
    s_have_collection = false;
    s_next_network_at = 0;
    report("error");
}

static bool paint_next_from_sd(void)
{
    int index = -1;
    if (!fc_play_next(s_manifest, &s_play, &index)) {
        report("paused");
        s_next_frame_at = 0;      /* repeat:once completed */
        return true;
    }
    const fc_frame_t *f = &s_manifest->frames[index];
    if (f->bytes != EPD_BUF_BYTES) {
        force_resync("cached frame dimensions do not match this panel");
        return false;
    }
    if (f->ttl_s > 0 &&
        collection_cache_frame_age_s(s_manifest->collection_id, f->digest) >
            f->ttl_s) {
        force_resync("cached frame expired");
        return false;
    }

    uint8_t *frame = NULL;
    if (!collection_cache_read_frame(s_manifest->collection_id, f->digest,
                                     f->bytes, &frame)) {
        force_resync("cached frame missing or corrupt");
        return false;
    }
    ESP_LOGI(TAG, "painting Album frame '%s' (%s) from SD, radio off",
             f->frame_id, f->digest);
    if (epd_port_init() != ESP_OK) {
        free(frame);
        force_resync("panel init failed during local playback");
        return false;
    }
    epd_init();
    epd_display(frame);
    epd_sleep();
    free(frame);
    report("playing");
    return true;
}

bool collection_try_local_cycle(void)
{
    if (!s_have_collection || !s_manifest ||
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return false;
    int64_t now = 0;
    if (!wall_clock_now(&now) || s_next_network_at <= now) return false;

    if (!s_play.done && (s_next_frame_at == 0 || s_next_frame_at <= now)) {
        if (!paint_next_from_sd()) return false;
        if (!s_play.done) s_next_frame_at = now + album_interval();
    }
    /* A slow e-paper refresh may have crossed the heartbeat deadline. Re-read
     * the RTC after painting so we do not accidentally sleep through it. */
    if (!wall_clock_now(&now)) return false;
    return s_next_network_at > now;
}

static bool same_string(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static void clear_assignment(void)
{
    const rest_config_t *c = rest_config_get();
    if (!c->collection_id[0] && !c->collection_synced_ver[0] &&
        !c->collection_srv_ver[0]) return;
    ESP_LOGI(TAG, "collection unbound; stopping offline playback");
    rest_config_set_collection_id("");
    rest_config_set_collection_synced_ver("");
    rest_config_set_collection_srv_ver("");
    rest_config_save();
    s_have_collection = false;
    schedule_reset();
    rest_set_collection_report(NULL, NULL, 0, 0, NULL);
}

bool collection_sync_pending(bool present, const char *id, const char *kind,
                             const char *version)
{
    if (!sdcard_mounted()) return false;
    if (!present) { clear_assignment(); return false; }
    if (!id || !id[0] || !version || !version[0] ||
        !kind || strcmp(kind, "album") != 0) {
        report("error");
        return false;
    }

    const rest_config_t *c = rest_config_get();
    bool id_changed = !same_string(c->collection_id, id);
    bool srv_changed = !same_string(c->collection_srv_ver, version);
    if (id_changed) {
        rest_config_set_collection_id(id);
        rest_config_set_collection_synced_ver("");
        s_have_collection = false;
        schedule_reset();
    }
    if (id_changed || srv_changed) {
        rest_config_set_collection_srv_ver(version);
        rest_config_save();
    }
    bool pending = id_changed || srv_changed ||
                   !same_string(rest_config_get()->collection_synced_ver, version) ||
                   !s_have_collection;
    if (pending) report("syncing");
    return pending;
}

static const fc_frame_t *find_frame(const fc_manifest_t *m, const char *digest)
{
    return fc_find_digest(m, digest);
}

static void resolve_collection_url(const char *relative, char *out, size_t cap)
{
    const char *server = rest_config_get()->server_url;
    snprintf(out, cap, "%s%s%s", server,
             relative && relative[0] == '/' ? "" : "/",
             relative ? relative : "");
}

void collection_sync_tail(bool present, const char *id, const char *kind,
                          const char *version)
{
    if (!collection_sync_pending(present, id, kind, version)) return;

    char *json = malloc(COLLECTION_MANIFEST_BUF);
    if (!json) { report("error"); return; }
    size_t len = 0;
    rest_status_t st = rest_get_collection_manifest(
        json, COLLECTION_MANIFEST_BUF, &len, 15000);
    if (st == REST_NO_CONTENT) {
        free(json);
        clear_assignment();
        return;
    }
    if (st != REST_OK) {
        ESP_LOGW(TAG, "collection manifest fetch failed (%d)", st);
        free(json);
        report("error");
        return;
    }

    fc_manifest_t *m = malloc(sizeof *m);
    if (!m || !fc_manifest_parse(json, len, m) ||
        strcmp(m->collection_id, id) != 0 ||
        strcmp(m->kind, kind) != 0 || strcmp(m->version, version) != 0) {
        ESP_LOGW(TAG, "collection manifest invalid or status envelope moved");
        free(json); free(m);
        report("error");
        return;
    }
    for (int i = 0; i < m->n_frames; i++) {
        if (m->frames[i].bytes != EPD_BUF_BYTES) {
            ESP_LOGW(TAG, "frame '%s' is %u bytes; panel requires %u",
                     m->frames[i].frame_id, (unsigned)m->frames[i].bytes,
                     (unsigned)EPD_BUF_BYTES);
            free(json); free(m);
            report("error");
            return;
        }
    }

    char have[FC_MAX_FRAMES][FC_DIGEST_HEX + 1];
    char fetch[FC_MAX_FRAMES][FC_DIGEST_HEX + 1];
    char orphan[FC_MAX_FRAMES][FC_DIGEST_HEX + 1];
    int n_orphan = 0;
    int n_have = collection_cache_list(m->collection_id, have, FC_MAX_FRAMES);
    int n_fetch = fc_sync_plan(m, have, n_have, fetch, FC_MAX_FRAMES,
                               orphan, FC_MAX_FRAMES, &n_orphan);
    ESP_LOGI(TAG, "sync '%s' -> v%s: %d cached, %d fetch, %d orphan",
             m->collection_id, m->version, n_have, n_fetch, n_orphan);

    bool complete = true;
    for (int i = 0; i < n_fetch; i++) {
        const fc_frame_t *f = find_frame(m, fetch[i]);
        if (!f) { complete = false; break; }
        char url[384];
        resolve_collection_url(f->url, url, sizeof url);
        fetched_image_t img;
        esp_err_t err = image_fetch_auth(url, rest_bearer_token(), &img);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "collection frame %s fetch failed: %s",
                     f->digest, esp_err_to_name(err));
            complete = false;
            break;
        }
        bool ok = collection_cache_write_frame(m->collection_id, f->digest,
                                               img.data, img.len, f->bytes);
        free(img.data);
        if (!ok) { complete = false; break; }
    }

    /* Publish the manifest only after every required blob is durable. Keep the
     * previous manifest/orphans intact on partial failure for recovery. */
    if (complete)
        complete = collection_cache_save_manifest(m->collection_id, json, len);
    if (complete)
        for (int i = 0; i < n_orphan; i++)
            collection_cache_delete(m->collection_id, orphan[i]);
    free(json);

    if (!complete) {
        free(m);
        report("error");
        return;
    }

    rest_config_set_collection_id(m->collection_id);
    rest_config_set_collection_srv_ver(m->version);
    rest_config_set_collection_synced_ver(m->version);
    rest_config_save();
    if (s_manifest) free(s_manifest);
    s_manifest = m;
    s_have_collection = true;
    fc_play_reset(&s_play, m->version, esp_random());
    int64_t now = 0;
    s_next_frame_at = wall_clock_now(&now) ? now + album_interval() : 0;
    report("playing");
    ESP_LOGI(TAG, "offline Album '%s' synced at v%s (%d/%ld frames)",
             m->collection_id, m->version, m->n_frames,
             (long)m->total_frames);
}

void collection_network_polled(int32_t normal_poll_s)
{
    if (!rest_config_get()->collection_id[0]) return;
    int64_t now = 0;
    if (!wall_clock_now(&now)) { s_next_network_at = 0; return; }
    int32_t poll = fc_interval_clamp(normal_poll_s, SLEEP_INTERVAL_MIN_S,
                                     SLEEP_INTERVAL_MAX_S, SLEEP_INTERVAL_S);
    s_next_network_at = now + poll;
    if (s_have_collection && !s_play.done && s_next_frame_at == 0)
        s_next_frame_at = now + album_interval();
}

void collection_network_painted(const char *digest)
{
    if (!s_have_collection || !s_manifest) return;
    if (digest && digest[0]) {
        const fc_frame_t *f = fc_find_digest(s_manifest, digest);
        if (f) {
            int index = (int)(f - s_manifest->frames);
            s_play.current_index = (int16_t)index;
            if (s_manifest->album.mode == FC_MODE_SHUFFLE)
                s_play.used_mask |= 1u << index;
        }
    }
    int64_t now = 0;
    s_next_frame_at = wall_clock_now(&now) ? now + album_interval() : 0;
    s_play.done = false;
    report("playing");
}

int32_t collection_next_sleep_s(int32_t ordinary_s)
{
    if (!s_have_collection || !s_manifest) return ordinary_s;
    int64_t now = 0;
    if (!wall_clock_now(&now)) return ordinary_s;

    int64_t best = ordinary_s;
    if (s_next_network_at > now && s_next_network_at - now < best)
        best = s_next_network_at - now;
    if (!s_play.done && s_next_frame_at > now && s_next_frame_at - now < best)
        best = s_next_frame_at - now;
    else if (!s_play.done && s_next_frame_at > 0 && s_next_frame_at <= now)
        best = SLEEP_INTERVAL_MIN_S; /* heartbeat won this wake; play next */
    if (best < SLEEP_INTERVAL_MIN_S) best = SLEEP_INTERVAL_MIN_S;
    if (best > SLEEP_INTERVAL_MAX_S) best = SLEEP_INTERVAL_MAX_S;
    return (int32_t)best;
}
