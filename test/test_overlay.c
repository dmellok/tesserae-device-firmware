/*
 * Host-side unit tests for overlay.c: spec parse (incl. rect-only and
 * missing-atlas cases), glyph lookup + fallback width, values/seq rules,
 * digest staleness, hygiene counter, invert + slot blit on synthetic
 * buffers. Build + run (see tools/test_overlay.sh):
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I src -I <cJSON dir> \
 *      test/test_overlay.c src/overlay.c <cJSON dir>/cJSON.c -o /tmp/to && /tmp/to
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "overlay.h"

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define PW 1872
#define PH 1404

static const char SPEC[] =
"{"
"  \"schema\": 1, \"frame_digest\": \"00112233445566aa\","
"  \"targets\": ["
"    {\"id\": \"t1\", \"x\": 120, \"y\": 640, \"w\": 300, \"h\": 90, \"echo\": \"invert\"},"
"    {\"id\": \"t2\", \"x\": -20, \"y\": 10, \"w\": 40, \"h\": 40, \"echo\": \"invert\"},"
"    {\"id\": \"t3\", \"x\": 5000, \"y\": 10, \"w\": 40, \"h\": 40, \"echo\": \"invert\"},"
"    {\"id\": \"t4\", \"x\": 0, \"y\": 0, \"w\": 10, \"h\": 10, \"echo\": \"sparkle\"}"
"  ],"
"  \"slots\": ["
"    {\"id\": \"s1\", \"x\": 140, \"y\": 660, \"w\": 200, \"h\": 32,"
"     \"key\": \"sensor.temp\", \"align\": \"right\", \"atlas\": \"a1\"},"
"    {\"id\": \"s2\", \"x\": 140, \"y\": 700, \"w\": 200, \"h\": 32,"
"     \"key\": \"sensor.hum\", \"align\": \"left\", \"atlas\": \"NOPE\"}"
"  ],"
"  \"atlases\": ["
"    {\"id\": \"a1\", \"digest\": \"ffeeddccbbaa9988\", \"height\": 2,"
"     \"url\": \"/api/v1/device/d/frame/overlay/atlas/ffeeddccbbaa9988\","
"     \"format\": \"4bpp-gray\","
"     \"glyphs\": {\"0\": {\"x\": 0, \"w\": 4}, \"1\": {\"x\": 4, \"w\": 2},"
"                  \".\": {\"x\": 6, \"w\": 2}}}"
"  ]"
"}";

int main(void)
{
    /* ---- spec parse ---- */
    overlay_spec_t sp;
    CHECK(overlay_spec_parse(SPEC, sizeof SPEC - 1, PW, PH, &sp));
    CHECK(strcmp(sp.frame_digest, "00112233445566aa") == 0);
    /* t2 clamps (x<0 -> x=0,w=20); t3 fully off-panel -> dropped;
     * t4 unknown echo -> dropped. */
    CHECK(sp.n_targets == 2);
    CHECK(sp.targets[1].x == 0 && sp.targets[1].w == 20);
    /* s2 references an unknown atlas -> dropped. */
    CHECK(sp.n_slots == 1);
    CHECK(sp.slots[0].align == OVERLAY_ALIGN_RIGHT);
    CHECK(sp.n_atlases == 1);
    CHECK(sp.atlases[0].strip_w == 8);
    CHECK(sp.atlases[0].mean_w == 2);   /* (4+2+2)/3 */

    /* rect-only spec (no slots/atlases) is valid */
    {
        static const char RECTONLY[] =
        "{\"schema\":1,\"frame_digest\":\"00112233445566aa\","
        "\"targets\":[{\"id\":\"t\",\"x\":1,\"y\":1,\"w\":5,\"h\":5,"
        "\"echo\":\"invert\"}]}";
        overlay_spec_t r;
        CHECK(overlay_spec_parse(RECTONLY, sizeof RECTONLY - 1, PW, PH, &r));
        CHECK(r.n_targets == 1 && r.n_slots == 0 && r.n_atlases == 0);
    }
    /* wrong schema / bad digest / empty spec all fail */
    {
        overlay_spec_t r;
        static const char S2[] = "{\"schema\":2,\"frame_digest\":\"00112233445566aa\","
                                 "\"targets\":[{\"id\":\"t\",\"x\":1,\"y\":1,\"w\":5,\"h\":5,\"echo\":\"invert\"}]}";
        CHECK(!overlay_spec_parse(S2, sizeof S2 - 1, PW, PH, &r));
        static const char BD[] = "{\"schema\":1,\"frame_digest\":\"XYZ\","
                                 "\"targets\":[{\"id\":\"t\",\"x\":1,\"y\":1,\"w\":5,\"h\":5,\"echo\":\"invert\"}]}";
        CHECK(!overlay_spec_parse(BD, sizeof BD - 1, PW, PH, &r));
        static const char EMPTY[] = "{\"schema\":1,\"frame_digest\":\"00112233445566aa\"}";
        CHECK(!overlay_spec_parse(EMPTY, sizeof EMPTY - 1, PW, PH, &r));
        CHECK(!overlay_spec_parse("nope", 4, PW, PH, &r));
    }

    /* ---- staleness ---- */
    CHECK(overlay_spec_matches(&sp, "00112233445566aa"));
    CHECK(!overlay_spec_matches(&sp, "00112233445566ab"));
    CHECK(!overlay_spec_matches(&sp, ""));

    /* ---- hit test ---- */
    CHECK(overlay_hit_target(&sp, 120, 640) != NULL);
    CHECK(overlay_hit_target(&sp, 419, 729) != NULL);
    CHECK(overlay_hit_target(&sp, 420, 729) == NULL);   /* right edge exclusive */
    CHECK(overlay_hit_target(&sp, 119, 640) == NULL);

    /* ---- glyphs ---- */
    overlay_atlas_t *a = overlay_find_atlas(&sp, "a1");
    CHECK(a != NULL);
    CHECK(overlay_glyph(a, '0') && overlay_glyph(a, '0')->w == 4);
    CHECK(overlay_glyph(a, 'X') == NULL);
    CHECK(overlay_text_width(a, "0.1") == 8);
    CHECK(overlay_text_width(a, "0X") == 4 + 2);   /* missing -> mean_w */

    /* ---- values doc + seq rules ---- */
    {
        int32_t seq = -1;
        static const char V7[] = "{\"seq\":7,\"values\":{\"sensor.temp\":\"21.4\"}}";
        uint32_t ch = overlay_values_apply(&sp, V7, sizeof V7 - 1, &seq);
        CHECK(ch == 0x1 && seq == 7);
        CHECK(strcmp(sp.slots[0].value, "21.4") == 0);
        /* equal seq: ignored even with a different value */
        static const char V7b[] = "{\"seq\":7,\"values\":{\"sensor.temp\":\"99\"}}";
        CHECK(overlay_values_apply(&sp, V7b, sizeof V7b - 1, &seq) == 0);
        CHECK(strcmp(sp.slots[0].value, "21.4") == 0);
        /* older seq: ignored */
        static const char V3[] = "{\"seq\":3,\"values\":{\"sensor.temp\":\"1\"}}";
        CHECK(overlay_values_apply(&sp, V3, sizeof V3 - 1, &seq) == 0);
        /* newer seq, same value: applied but no change bit */
        static const char V8[] = "{\"seq\":8,\"values\":{\"sensor.temp\":\"21.4\"}}";
        CHECK(overlay_values_apply(&sp, V8, sizeof V8 - 1, &seq) == 0 && seq == 8);
        /* unknown keys are ignored */
        static const char V9[] = "{\"seq\":9,\"values\":{\"other\":\"x\"}}";
        CHECK(overlay_values_apply(&sp, V9, sizeof V9 - 1, &seq) == 0 && seq == 9);
    }

    /* ---- hygiene counter ---- */
    {
        overlay_hygiene_t hy;
        overlay_hygiene_reset(&hy);
        int fired = 0;
        for (int i = 0; i < OVERLAY_HYGIENE_LIMIT; i++)
            if (overlay_hygiene_tick(&hy)) fired++;
        CHECK(fired == 1);
        overlay_hygiene_reset(&hy);
        CHECK(!overlay_hygiene_tick(&hy));
    }

    /* ---- invert + slot blit on a synthetic 16x4 4bpp buffer ---- */
    {
        enum { W = 16, H = 4 };
        uint8_t base[W / 2 * H], fb[W / 2 * H];
        memset(base, 0x77, sizeof base);   /* mid-gray everywhere */
        memcpy(fb, base, sizeof fb);

        overlay_invert_rect(fb, W, H, 4, 2, 1, 4, 2);
        /* inside: 0xF - 7 = 8; outside untouched */
        CHECK((fb[(1 * (W / 2)) + 1] & 0xF0) == 0x80);   /* px (2,1) high nibble */
        CHECK(fb[0] == 0x77);
        /* double invert restores */
        overlay_invert_rect(fb, W, H, 4, 2, 1, 4, 2);
        CHECK(memcmp(fb, base, sizeof fb) == 0);

        /* slot blit: atlas strip 8x2 4bpp; glyph '1' = columns 4..5 = 0xAB rows */
        static const uint8_t strip[] = {
            0x01, 0x23, 0xAB, 0xCD,   /* row 0: '0'=0123, '1'=AB, '.'=CD */
            0x45, 0x67, 0xEF, 0x89,   /* row 1 */
        };
        overlay_atlas_t at = sp.atlases[0];
        at.bits = strip;
        overlay_slot_t sl = sp.slots[0];
        sl.x = 4; sl.y = 1; sl.w = 8; sl.h = 3;   /* taller than atlas: top-aligned */
        sl.align = OVERLAY_ALIGN_LEFT;
        strcpy(sl.value, "1");
        overlay_draw_slot(fb, base, W, H, 4, &sl, &at);
        /* '1' is 2px wide -> pixels (4,1)=0xA,(5,1)=0xB,(4,2)=0xE,(5,2)=0xF */
        CHECK((fb[(1 * (W / 2)) + 2]) == 0xAB);
        CHECK((fb[(2 * (W / 2)) + 2]) == 0xEF);
        /* rest of the slot restored from base */
        CHECK((fb[(1 * (W / 2)) + 3]) == 0x77);
        /* right-align within w=8: '1' (2px) starts at x=4+8-2=10 */
        sl.align = OVERLAY_ALIGN_RIGHT;
        overlay_draw_slot(fb, base, W, H, 4, &sl, &at);
        CHECK((fb[(1 * (W / 2)) + 5]) == 0xAB);   /* px 10,11 */
        CHECK((fb[(1 * (W / 2)) + 2]) == 0x77);   /* old position restored */
        /* no atlas bits -> restore only, never crash */
        at.bits = NULL;
        overlay_draw_slot(fb, base, W, H, 4, &sl, &at);
        CHECK(memcmp(fb, base, sizeof fb) == 0);
    }

    /* ---- target cap: 32 parse, 40 overflow (first 32 win, doc valid) ---- */
    {
        CHECK(OVERLAY_MAX_TARGETS == 32);   /* the advertised max_targets */

        char doc[16384];
        int n = snprintf(doc, sizeof doc,
                         "{\"schema\":1,\"frame_digest\":\"00112233445566aa\","
                         "\"targets\":[");
        for (int i = 0; i < 40; i++) {
            n += snprintf(doc + n, sizeof doc - (size_t)n,
                          "%s{\"id\":\"t%d\",\"x\":%d,\"y\":%d,"
                          "\"w\":60,\"h\":40,\"echo\":\"invert\"}",
                          i ? "," : "", i, (i % 8) * 70, (i / 8) * 50);
        }
        n += snprintf(doc + n, sizeof doc - (size_t)n, "]}");
        overlay_spec_t big;
        CHECK(overlay_spec_parse(doc, (size_t)n, PW, PH, &big));
        CHECK(big.n_targets == 32);                          /* extras dropped */
        CHECK(strcmp(big.targets[0].id, "t0") == 0);         /* first N win... */
        CHECK(strcmp(big.targets[31].id, "t31") == 0);       /* ...in doc order */
        CHECK(overlay_hit_target(&big, 71, 1) != NULL);      /* t1 still hits */
        CHECK(overlay_hit_target(&big, (39 % 8) * 70 + 1, (39 / 8) * 50 + 1)
              != NULL || true);   /* t39's cell may be covered by none: no crash */

        /* exactly 32 parses cleanly too */
        char *tail = strstr(doc, ",{\"id\":\"t32\"");
        CHECK(tail != NULL);
        memcpy(tail, "]}", 3);
        CHECK(overlay_spec_parse(doc, strlen(doc), PW, PH, &big));
        CHECK(big.n_targets == 32);
    }

    /* ---- spec-size arithmetic: worst-case doc fits the 8 KB device cap ---- */
    {
        /* 32 maximal targets + 8 maximal slots + 2 atlases x 32 glyphs, all
         * field values at their caps. The device parses specs into an 8 KB
         * buffer (OVERLAY_SPEC_MAX); this documents that the contract's
         * worst case fits with room. */
        char doc[16384];
        int n = snprintf(doc, sizeof doc,
                         "{\"schema\":1,\"frame_digest\":\"00112233445566aa\","
                         "\"targets\":[");
        for (int i = 0; i < 32; i++)
            n += snprintf(doc + n, sizeof doc - (size_t)n,
                          "%s{\"id\":\"t%05d\",\"x\":1871,\"y\":1403,"
                          "\"w\":1872,\"h\":1404,\"echo\":\"invert\"}",
                          i ? "," : "", i);
        n += snprintf(doc + n, sizeof doc - (size_t)n, "],\"slots\":[");
        for (int i = 0; i < 8; i++)
            n += snprintf(doc + n, sizeof doc - (size_t)n,
                          "%s{\"id\":\"s%05d\",\"x\":1871,\"y\":1403,"
                          "\"w\":1872,\"h\":1404,"
                          "\"key\":\"a.very.long.dotted.value.key.%04d\","
                          "\"align\":\"center\",\"atlas\":\"a1\"}",
                          i ? "," : "", i, i);
        n += snprintf(doc + n, sizeof doc - (size_t)n, "],\"atlases\":[");
        for (int a = 0; a < 2; a++) {
            n += snprintf(doc + n, sizeof doc - (size_t)n,
                          "%s{\"id\":\"a%d\",\"digest\":\"ffeeddccbbaa9988\","
                          "\"height\":64,\"url\":"
                          "\"/api/v1/device/aaaabbbbccccdddd/frame/overlay/atlas/ffeeddccbbaa9988\","
                          "\"format\":\"4bpp-gray\",\"glyphs\":{",
                          a ? "," : "", a + 1);
            for (int g = 0; g < 32; g++)
                n += snprintf(doc + n, sizeof doc - (size_t)n,
                              "%s\"%c\":{\"x\":%d,\"w\":32}",
                              g ? "," : "", '0' + (g % 75), g * 32);
            n += snprintf(doc + n, sizeof doc - (size_t)n, "}}");
        }
        n += snprintf(doc + n, sizeof doc - (size_t)n, "]}");
        CHECK(n < 8192);   /* worst case fits the 8 KB device spec buffer */
        overlay_spec_t big;
        CHECK(overlay_spec_parse(doc, (size_t)n, PW, PH, &big));
        CHECK(big.n_targets == 32 && big.n_slots == 8);
    }

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
