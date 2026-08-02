/* Host tests for the pinned frame-cache manifest and album playback policy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_collection.h"
#include "cJSON.h"

static const char MANIFEST[] =
"{"
"\"schema\":1,\"collection_id\":\"album:kitchen\",\"kind\":\"album\","
"\"version\":\"a1b2c3d4e5f60718\",\"total_frames\":3,"
"\"cursor\":null,\"next_cursor\":null,\"frames\":["
"{\"frame_id\":\"photo:b\",\"position\":1,\"digest\":\"1111111111111111\","
" \"bytes\":960000,\"ttl_s\":0,\"cache\":true,"
" \"url\":\"/api/v1/device/frame01/collection/frame/1111111111111111\"},"
"{\"frame_id\":\"photo:a\",\"position\":0,\"digest\":\"0000000000000000\","
" \"bytes\":960000,\"ttl_s\":0,\"cache\":true,"
" \"url\":\"/api/v1/device/frame01/collection/frame/0000000000000000\"},"
"{\"frame_id\":\"photo:c\",\"position\":2,\"digest\":\"2222222222222222\","
" \"bytes\":960000,\"ttl_s\":0,\"cache\":false,"
" \"url\":\"/api/v1/device/frame01/collection/frame/2222222222222222\"}],"
"\"producer\":{\"album\":{\"playback\":{\"mode\":\"shuffle\","
"\"interval_s\":1800,\"repeat\":\"reshuffle\"}}}}";

static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static void replace_once(char *buf, const char *from, const char *to)
{
    char *p = strstr(buf, from);
    if (p) memcpy(p, to, strlen(from));
}

static char *sized_manifest(int listed, int cache_true)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "collection_id", "album:limit");
    cJSON_AddStringToObject(root, "kind", "album");
    cJSON_AddStringToObject(root, "version", "abcdef0123456789");
    cJSON_AddNumberToObject(root, "total_frames", listed);
    cJSON_AddNullToObject(root, "cursor");
    cJSON_AddNullToObject(root, "next_cursor");
    cJSON *frames = cJSON_AddArrayToObject(root, "frames");
    for (int i = 0; i < listed; i++) {
        char id[24], digest[FC_DIGEST_HEX + 1], url[128];
        snprintf(id, sizeof id, "photo:%d", i);
        snprintf(digest, sizeof digest, "%016x", i);
        snprintf(url, sizeof url, "/api/v1/device/d/collection/frame/%s", digest);
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "frame_id", id);
        cJSON_AddNumberToObject(f, "position", i);
        cJSON_AddStringToObject(f, "digest", digest);
        cJSON_AddNumberToObject(f, "bytes", 960000);
        cJSON_AddNumberToObject(f, "ttl_s", 0);
        cJSON_AddBoolToObject(f, "cache", i < cache_true);
        cJSON_AddStringToObject(f, "url", url);
        cJSON_AddItemToArray(frames, f);
    }
    cJSON *producer = cJSON_AddObjectToObject(root, "producer");
    cJSON *album = cJSON_AddObjectToObject(producer, "album");
    cJSON *play = cJSON_AddObjectToObject(album, "playback");
    cJSON_AddStringToObject(play, "mode", "shuffle");
    cJSON_AddNumberToObject(play, "interval_s", 1800);
    cJSON_AddStringToObject(play, "repeat", "reshuffle");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *read_fixture(const char *path, size_t *length)
{
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *json = malloc((size_t)size + 1);
    if (!json) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(json, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        free(json);
        return NULL;
    }
    json[got] = '\0';
    *length = got;
    return json;
}

int main(int argc, char **argv)
{
    fc_manifest_t m;
    if (argc == 2) {
        size_t fixture_len = 0;
        char *fixture = read_fixture(argv[1], &fixture_len);
        bool parsed = false;
        CHECK(fixture != NULL);
        if (fixture) parsed = fc_manifest_parse(fixture, fixture_len, &m);
        CHECK(parsed);
        if (parsed) {
            printf("frame_collection fixture: %s (%d retained frame(s))\n",
                   argv[1], m.n_frames);
        }
        free(fixture);
    }
    CHECK(fc_manifest_parse(MANIFEST, sizeof MANIFEST - 1, &m));
    CHECK(m.schema == 1);
    CHECK(strcmp(m.collection_id, "album:kitchen") == 0);
    CHECK(strcmp(m.kind, "album") == 0);
    CHECK(m.total_frames == 3 && m.n_frames == 2);
    CHECK(strcmp(m.frames[0].frame_id, "photo:a") == 0); /* sorted by position */
    CHECK(strcmp(m.frames[1].frame_id, "photo:b") == 0);
    CHECK(m.album.mode == FC_MODE_SHUFFLE);
    CHECK(m.album.repeat == FC_REPEAT_RESHUFFLE);
    CHECK(m.album.interval_s == 1800);
    CHECK(fc_find_frame_id(&m, "photo:a") == &m.frames[0]);
    CHECK(fc_find_digest(&m, "1111111111111111") == &m.frames[1]);
    CHECK(fc_find_frame_id(&m, "photo:c") == NULL); /* cache:false not retained */

    CHECK(fc_digest_valid("0123456789abcdef"));
    CHECK(!fc_digest_valid("0123456789abcdeF"));
    CHECK(!fc_digest_valid("short"));

    {
        char bad[sizeof MANIFEST]; memcpy(bad, MANIFEST, sizeof bad);
        replace_once(bad, "\"schema\":1", "\"schema\":2");
        CHECK(!fc_manifest_parse(bad, strlen(bad), &m));
    }
    {
        char bad[sizeof MANIFEST]; memcpy(bad, MANIFEST, sizeof bad);
        replace_once(bad, "a1b2c3d4e5f60718", "A1b2c3d4e5f60718");
        CHECK(!fc_manifest_parse(bad, strlen(bad), &m));
    }
    {
        char bad[sizeof MANIFEST]; memcpy(bad, MANIFEST, sizeof bad);
        replace_once(bad, "\"mode\":\"shuffle\"", "\"mode\":\"sideways\"");
        CHECK(!fc_manifest_parse(bad, strlen(bad), &m));
    }
    CHECK(!fc_manifest_parse(NULL, 0, &m));

    /* The advertised slice-1 boundary is exactly 32 retained frames. A larger
     * total is parseable only when overflow entries are cache:false. */
    {
        char *json = sized_manifest(32, 32);
        CHECK(json && fc_manifest_parse(json, strlen(json), &m));
        CHECK(m.n_frames == 32 && m.total_frames == 32);
        free(json);
        json = sized_manifest(33, 33);
        CHECK(json && !fc_manifest_parse(json, strlen(json), &m));
        free(json);
        json = sized_manifest(40, 32);
        CHECK(json && fc_manifest_parse(json, strlen(json), &m));
        CHECK(m.n_frames == 32 && m.total_frames == 40);
        free(json);
    }

    CHECK(fc_manifest_parse(MANIFEST, sizeof MANIFEST - 1, &m));
    {
        char have[3][FC_DIGEST_HEX + 1] = {
            "0000000000000000", "eeeeeeeeeeeeeeee", ""
        };
        char fetch[FC_MAX_FRAMES][FC_DIGEST_HEX + 1] = {{0}};
        char orphan[FC_MAX_FRAMES][FC_DIGEST_HEX + 1] = {{0}};
        int no = -1;
        int nf = fc_sync_plan(&m, have, 2, fetch, FC_MAX_FRAMES,
                              orphan, FC_MAX_FRAMES, &no);
        CHECK(nf == 1 && strcmp(fetch[0], "1111111111111111") == 0);
        CHECK(no == 1 && strcmp(orphan[0], "eeeeeeeeeeeeeeee") == 0);
    }

    /* Sequential loop and once. */
    fc_play_state_t ps;
    int idx = -1;
    m.album.mode = FC_MODE_SEQUENTIAL;
    m.album.repeat = FC_REPEAT_LOOP;
    fc_play_reset(&ps, m.version, 1);
    CHECK(fc_play_next(&m, &ps, &idx) && idx == 0);
    CHECK(fc_play_next(&m, &ps, &idx) && idx == 1);
    CHECK(fc_play_next(&m, &ps, &idx) && idx == 0);
    m.album.repeat = FC_REPEAT_ONCE;
    fc_play_reset(&ps, m.version, 1);
    CHECK(fc_play_next(&m, &ps, &idx) && idx == 0);
    CHECK(fc_play_next(&m, &ps, &idx) && idx == 1);
    CHECK(!fc_play_next(&m, &ps, &idx) && ps.done);

    /* Shuffle visits each frame once; a fresh bag cannot immediately repeat. */
    m.album.mode = FC_MODE_SHUFFLE;
    m.album.repeat = FC_REPEAT_RESHUFFLE;
    fc_play_reset(&ps, m.version, 0x12345678);
    int first, second, third;
    CHECK(fc_play_next(&m, &ps, &first));
    CHECK(fc_play_next(&m, &ps, &second));
    CHECK(first != second);
    CHECK(fc_play_next(&m, &ps, &third));
    CHECK(third != second);

    /* Version change resets traversal while preserving a usable RNG seed. */
    strcpy(m.version, "ffffffffffffffff");
    CHECK(fc_play_next(&m, &ps, &idx));
    CHECK(strcmp(ps.version, m.version) == 0 && !ps.done);

    CHECK(fc_interval_clamp(10, 30, 86400, 1800) == 30);
    CHECK(fc_interval_clamp(90000, 30, 86400, 1800) == 86400);
    CHECK(fc_interval_clamp(600, 30, 86400, 1800) == 600);
    CHECK(fc_interval_clamp(0, 30, 86400, 1800) == 1800);

    printf("frame_collection: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
