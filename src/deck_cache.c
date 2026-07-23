/* deck_cache.c -- SD-card file layer for the deck frame cache. See header. */

#include "deck_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sdcard.h"

static const char *TAG = "deck_cache";

/* Manifests are small JSON; cap well above DECK_MAX_PAGES worth of content. */
#define DECK_MANIFEST_MAX (16 * 1024)

/* deck_id becomes a directory name on a user-readable card: restrict it to
 * filesystem-safe chars so a hostile server value cannot traverse paths. */
static bool deck_id_safe(const char *id)
{
    if (!id || !id[0] || strlen(id) >= DECK_ID_CAP) return false;
    for (const char *p = id; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '-' || *p == '_';
        if (!ok) return false;
    }
    return true;
}

static bool deck_dir(const char *deck_id, char *out, size_t cap)
{
    if (!sdcard_mounted() || !deck_id_safe(deck_id)) return false;
    int n = snprintf(out, cap, SDCARD_MOUNT_POINT "/tesserae/decks/%s", deck_id);
    return n > 0 && n < (int)cap;
}

/* mkdir -p for the three fixed levels; EEXIST is success. */
static bool ensure_deck_dir(const char *deck_id)
{
    char path[128];
    if (!deck_dir(deck_id, path, sizeof path)) return false;
    const char *levels[] = { SDCARD_MOUNT_POINT "/tesserae",
                             SDCARD_MOUNT_POINT "/tesserae/decks", path };
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        if (mkdir(levels[i], 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s failed (errno %d)", levels[i], errno);
            return false;
        }
    }
    return true;
}

bool deck_cache_load_manifest(const char *deck_id, deck_manifest_t *out)
{
    char path[160];
    if (!deck_dir(deck_id, path, sizeof path)) return false;
    strlcat(path, "/manifest.json", sizeof path);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    char *buf = malloc(DECK_MANIFEST_MAX);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, DECK_MANIFEST_MAX, f);
    bool full = feof(f);
    fclose(f);

    bool ok = full && n > 0 && deck_manifest_parse(buf, n, out);
    free(buf);
    if (!ok) ESP_LOGW(TAG, "cached manifest for %s unreadable; ignoring", deck_id);
    return ok;
}

bool deck_cache_save_manifest(const char *deck_id, const char *json, size_t len)
{
    if (!json || !len || len > DECK_MANIFEST_MAX) return false;
    if (!ensure_deck_dir(deck_id)) return false;

    char path[160], tmp[160];
    if (!deck_dir(deck_id, path, sizeof path)) return false;
    snprintf(tmp, sizeof tmp, "%s/manifest.tmp", path);
    strlcat(path, "/manifest.json", sizeof path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(json, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        unlink(path);            /* FATFS rename does not overwrite */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) { unlink(tmp); ESP_LOGW(TAG, "manifest save failed for %s", deck_id); }
    return ok;
}

static bool frame_path(const char *deck_id, const char *digest,
                       char *out, size_t cap)
{
    if (!deck_digest_valid(digest)) return false;
    char dir[128];
    if (!deck_dir(deck_id, dir, sizeof dir)) return false;
    int n = snprintf(out, cap, "%s/%s.bin", dir, digest);
    return n > 0 && n < (int)cap;
}

bool deck_cache_read_frame(const char *deck_id, const char *digest,
                           uint32_t expect_bytes, uint8_t **out)
{
    *out = NULL;
    char path[176];
    if (!frame_path(deck_id, digest, path, sizeof path)) return false;

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Frames are panel-sized; prefer PSRAM like the network fetch path. */
    uint8_t *buf = heap_caps_malloc(expect_bytes + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(expect_bytes + 1);
    if (!buf) { fclose(f); return false; }

    size_t n = fread(buf, 1, expect_bytes + 1, f);   /* +1 catches oversize */
    fclose(f);

    if (!deck_digest_check(buf, n, expect_bytes, digest)) {
        ESP_LOGW(TAG, "cached frame %s fails verification; deleting", digest);
        free(buf);
        unlink(path);
        return false;
    }
    *out = buf;
    return true;
}

bool deck_cache_write_frame(const char *deck_id, const char *digest,
                            const uint8_t *data, size_t len, uint32_t expect_bytes)
{
    if (!deck_digest_check(data, len, expect_bytes, digest)) {
        ESP_LOGW(TAG, "fetched frame %s fails verification; not caching", digest);
        return false;
    }
    if (!ensure_deck_dir(deck_id)) return false;

    char path[176], tmp[176];
    if (!frame_path(deck_id, digest, path, sizeof path)) return false;
    char dir[128];
    deck_dir(deck_id, dir, sizeof dir);
    snprintf(tmp, sizeof tmp, "%s/frame.tmp", dir);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) { unlink(tmp); ESP_LOGW(TAG, "frame write failed for %s", digest); }
    return ok;
}

int deck_cache_list(const char *deck_id,
                    char digests[][DECK_DIGEST_HEX + 1], int max)
{
    char dir[128];
    if (!deck_dir(deck_id, dir, sizeof dir)) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len != DECK_DIGEST_HEX + 4) continue;          /* "<16hex>.bin" */
        if (strcmp(e->d_name + DECK_DIGEST_HEX, ".bin") != 0) continue;
        char digest[DECK_DIGEST_HEX + 1];
        memcpy(digest, e->d_name, DECK_DIGEST_HEX);
        digest[DECK_DIGEST_HEX] = '\0';
        /* FATFS may report uppercase 8.3 names; normalise before validating. */
        for (int i = 0; i < DECK_DIGEST_HEX; i++)
            if (digest[i] >= 'A' && digest[i] <= 'F') digest[i] += 'a' - 'A';
        if (!deck_digest_valid(digest)) continue;
        strcpy(digests[n++], digest);
    }
    closedir(d);
    return n;
}

void deck_cache_delete(const char *deck_id, const char *digest)
{
    char path[176];
    if (frame_path(deck_id, digest, path, sizeof path)) unlink(path);
}

int32_t deck_cache_frame_age_s(const char *deck_id, const char *digest)
{
    char path[176];
    if (!frame_path(deck_id, digest, path, sizeof path)) return INT32_MAX;
    struct stat st;
    if (stat(path, &st) != 0) return INT32_MAX;
    time_t now = time(NULL);
    /* Both stamps must be plausible wall-clock (the RTC keeps time across
     * deep sleep; a fresh RESET before any network wake has no clock). */
    if (now < 1600000000 || st.st_mtime < 1600000000 || now < st.st_mtime)
        return INT32_MAX;
    return (int32_t)(now - st.st_mtime);
}
