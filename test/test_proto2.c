/*
 * Host-side unit tests for proto2.c: manifest parse (regions, downgrades,
 * text + atlas dedup, caps), gesture classification thresholds, hit-test
 * rules (first-hit-wins, slide absorption, swipe lift-point rescue, slide
 * value mapping), and the nibble-phase text blitter on a synthetic 4bpp
 * framebuffer. Build + run (see tools/test_proto2.sh):
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I src -I <cJSON dir> \
 *      test/test_proto2.c src/proto2.c <cJSON dir>/cJSON.c -o /tmp/tp && /tmp/tp
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "proto2.h"

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define PW 1872
#define PH 1404

/* The contract's example manifest, §4, condensed + edge cases appended. */
static const char MANIFEST[] =
"{"
"  \"proto\": 2,"
"  \"frame_digest\": \"8603c4372c14c7b9\","
"  \"manifest_digest\": \"5f1e9c22ab34cd01\","
"  \"regions\": ["
"    {\"id\": \"el:tile_desk:tap\","
"     \"rect\": {\"x\": 128, \"y\": 640, \"w\": 300, \"h\": 90},"
"     \"gestures\": {\"tap\": true},"
"     \"action\": {\"tier\": 1, \"type\": \"ha\"},"
"     \"feedback\": {\"mode\": \"tiles\", \"set\": \"st:tile_desk\","
"                    \"cycle\": [\"off\", \"on\"]}},"
"    {\"id\": \"el:nav_next:tap\","
"     \"rect\": {\"x\": 1700, \"y\": 0, \"w\": 172, \"h\": 172},"
"     \"gestures\": {\"tap\": true, \"swipe\": {\"left\": true}},"
"     \"action\": {\"tier\": 0, \"type\": \"nav\", \"target\": \"page:b2f1c3d4e5f60718\"},"
"     \"feedback\": {\"mode\": \"invert\"}},"
"    {\"id\": \"el:dimmer:slide\","
"     \"rect\": {\"x\": 100, \"y\": 900, \"w\": 60, \"h\": 400},"
"     \"gestures\": {\"slide\": {\"axis\": \"y\"}},"
"     \"action\": {\"tier\": 1, \"type\": \"ha\"},"
"     \"feedback\": {\"mode\": \"slider\", \"track\": \"vertical\","
"                    \"value_text\": \"tx:dim_pct\"}},"
"    {\"id\": \"mk:4f2a91cc\","
"     \"rect\": {\"x\": 0, \"y\": 1300, \"w\": 400, \"h\": 104},"
"     \"gestures\": {\"tap\": true},"
"     \"action\": {\"tier\": 2, \"type\": \"refresh\"},"
"     \"feedback\": {\"mode\": \"invert\"}},"
"    {\"id\": \"el:future:tap\","
"     \"rect\": {\"x\": 600, \"y\": 0, \"w\": 100, \"h\": 100},"
"     \"gestures\": {\"tap\": true},"
"     \"action\": {\"tier\": 0, \"type\": \"hologram\"},"
"     \"feedback\": {\"mode\": \"sparkle\"}},"
"    {\"id\": \"el:offpanel:tap\","
"     \"rect\": {\"x\": 5000, \"y\": 0, \"w\": 100, \"h\": 100},"
"     \"gestures\": {\"tap\": true},"
"     \"action\": {\"tier\": 2, \"type\": \"refresh\"},"
"     \"feedback\": {\"mode\": \"invert\"}},"
"    {\"id\": \"el:nogesture:x\","
"     \"rect\": {\"x\": 0, \"y\": 0, \"w\": 100, \"h\": 100},"
"     \"gestures\": {},"
"     \"action\": {\"tier\": 2, \"type\": \"refresh\"},"
"     \"feedback\": {\"mode\": \"invert\"}}"
"  ],"
"  \"text\": ["
"    {\"id\": \"tx:dim_pct\","
"     \"rect\": {\"x\": 180, \"y\": 1060, \"w\": 120, \"h\": 40},"
"     \"align\": \"right\","
"     \"atlas\": {\"digest\": \"ab12cd34ef56ab12\","
"                 \"url\": \"/api/v1/device/d/frame/overlay/atlas/ab12cd34ef56ab12\","
"                 \"height\": 32,"
"                 \"glyphs\": {\"0\": {\"x\": 0, \"w\": 18}, \"1\": {\"x\": 18, \"w\": 12},"
"                              \"%\": {\"x\": 30, \"w\": 20}}},"
"     \"key\": \"ha:light.desk:attributes.brightness_pct\","
"     \"max_chars\": 5},"
"    {\"id\": \"tx:same_atlas\","
"     \"rect\": {\"x\": 180, \"y\": 1110, \"w\": 120, \"h\": 40},"
"     \"align\": \"left\","
"     \"atlas\": {\"digest\": \"ab12cd34ef56ab12\","
"                 \"url\": \"/api/v1/device/d/frame/overlay/atlas/ab12cd34ef56ab12\","
"                 \"height\": 32,"
"                 \"glyphs\": {\"0\": {\"x\": 0, \"w\": 18}}},"
"     \"key\": \"local:clock:HH.MM\"},"
"    {\"id\": \"tx:bad_atlas\","
"     \"rect\": {\"x\": 0, \"y\": 0, \"w\": 100, \"h\": 40},"
"     \"atlas\": {\"digest\": \"NOT_A_DIGEST\", \"url\": \"/x\", \"height\": 32,"
"                 \"glyphs\": {\"0\": {\"x\": 0, \"w\": 4}}},"
"     \"key\": \"k\"}"
"  ],"
"  \"caps\": {\"max_regions\": 32, \"max_text\": 8, \"max_atlases\": 2},"
"  \"some_future_field\": [1, 2, 3]"
"}";

int main(void)
{
    /* ---- manifest parse ---- */
    static p2_manifest_t m;
    CHECK(p2_manifest_parse(MANIFEST, sizeof MANIFEST - 1, PW, PH, &m));
    CHECK(strcmp(m.frame_digest, "8603c4372c14c7b9") == 0);
    CHECK(strcmp(m.manifest_digest, "5f1e9c22ab34cd01") == 0);
    CHECK(p2_manifest_matches(&m, "8603c4372c14c7b9"));
    CHECK(!p2_manifest_matches(&m, "0000000000000000"));

    /* offpanel + no-gesture regions dropped; the rest (incl. downgraded
     * future region) survive in document order. */
    CHECK(m.n_regions == 5);
    CHECK(strcmp(m.regions[0].id, "el:tile_desk:tap") == 0);
    CHECK(m.regions[0].tier == 1 && m.regions[0].type == P2_ACT_HA);
    CHECK(m.regions[0].fb == P2_FB_TILES && m.regions[0].n_cycle == 2);
    CHECK(strcmp(m.regions[0].fb_set, "st:tile_desk") == 0);
    CHECK(strcmp(m.regions[0].cycle[1], "on") == 0);

    CHECK(m.regions[1].tier == 0 && m.regions[1].type == P2_ACT_NAV);
    CHECK(strcmp(m.regions[1].target, "page:b2f1c3d4e5f60718") == 0);
    CHECK(m.regions[1].g_tap && (m.regions[1].g_swipe & P2_SW_LEFT));

    CHECK(m.regions[2].g_slide && m.regions[2].slide_axis == 'y');
    CHECK(m.regions[2].fb == P2_FB_SLIDER && m.regions[2].fb_track == 'v');
    CHECK(strcmp(m.regions[2].fb_value_text, "tx:dim_pct") == 0);

    CHECK(m.regions[3].type == P2_ACT_REFRESH && m.regions[3].tier == 2);

    /* unknown type + mode -> downgraded, never dropped */
    CHECK(m.regions[4].type == P2_ACT_UNKNOWN);
    CHECK(m.regions[4].fb == P2_FB_INVERT);
    CHECK(m.regions[4].tier == 2);

    /* text: bad atlas dropped, shared atlas deduped to one slot */
    CHECK(m.n_text == 2);
    CHECK(m.n_atlases == 1);
    CHECK(m.text[0].atlas_idx == 0 && m.text[1].atlas_idx == 0);
    CHECK(m.text[0].align == 2 && m.text[1].align == 0);
    CHECK(m.text[0].max_chars == 5);
    CHECK(strcmp(m.text[1].key, "local:clock:HH.MM") == 0);
    CHECK(m.atlases[0].n_glyphs == 3);          /* first-parsed table wins */
    CHECK(m.atlases[0].strip_w == 50);
    CHECK(m.atlases[0].mean_w == 16);           /* (18+12+20)/3 */

    /* ---- strict rejects ---- */
    static p2_manifest_t bad;
    CHECK(!p2_manifest_parse("{\"proto\": 1, \"frame_digest\": \"8603c4372c14c7b9\","
                             "\"manifest_digest\": \"5f1e9c22ab34cd01\"}",
                             90, PW, PH, &bad));
    CHECK(!p2_manifest_parse("{\"proto\": 2, \"frame_digest\": \"XYZ\","
                             "\"manifest_digest\": \"5f1e9c22ab34cd01\"}",
                             80, PW, PH, &bad));
    /* no regions/text at all is still a valid (empty) manifest */
    static const char EMPTY[] =
        "{\"proto\": 2, \"frame_digest\": \"8603c4372c14c7b9\","
        "\"manifest_digest\": \"5f1e9c22ab34cd01\"}";
    CHECK(p2_manifest_parse(EMPTY, sizeof EMPTY - 1, PW, PH, &bad));
    CHECK(bad.n_regions == 0 && bad.n_text == 0);

    /* ---- gesture classification ---- */
    CHECK(p2_classify(100, 100, 100, 100, 80) == P2_G_TAP);
    CHECK(p2_classify(100, 100, 110, 105, 350) == P2_G_TAP);
    /* long-press still taps (documented deviation) */
    CHECK(p2_classify(100, 100, 102, 102, 900) == P2_G_TAP);
    /* dead zone 20-40 px */
    CHECK(p2_classify(100, 100, 130, 100, 200) == P2_G_NONE);
    /* swipes, dominant axis + sign */
    CHECK(p2_classify(500, 500, 440, 510, 200) == P2_G_SWIPE_LEFT);
    CHECK(p2_classify(500, 500, 560, 490, 200) == P2_G_SWIPE_RIGHT);
    CHECK(p2_classify(500, 500, 505, 430, 200) == P2_G_SWIPE_UP);
    CHECK(p2_classify(500, 500, 495, 580, 200) == P2_G_SWIPE_DOWN);
    /* horizontal wins the diagonal tie */
    CHECK(p2_classify(0, 0, 50, 50, 200) == P2_G_SWIPE_RIGHT);

    /* ---- hit test ---- */
    p2_gesture_t g;
    int val = -1;

    /* tap inside the tile region */
    CHECK(p2_hit(&m, 200, 690, 200, 690, 100, &g, &val) == 0);
    CHECK(g == P2_G_TAP);

    /* tap that hits nothing */
    CHECK(p2_hit(&m, 900, 700, 900, 700, 100, &g, &val) == -1);

    /* swipe-left starting in the nav region */
    CHECK(p2_hit(&m, 1750, 80, 1600, 80, 200, &g, &val) == 1);
    CHECK(g == P2_G_SWIPE_LEFT);

    /* swipe-left starting OUTSIDE, lifting INSIDE the nav region: rescued */
    CHECK(p2_hit(&m, 1690, 80, 1710, 80, 200, &g, &val) == -1); /* 20px: dead */
    /* a RIGHT swipe lifting inside the zone must MISS: nav declares left
     * only, and pass 2 requires the matching direction. */
    CHECK(p2_hit(&m, 1550, 80, 1750, 80, 200, &g, &val) == -1);
    CHECK(p2_hit(&m, 1860, 300, 1790, 100, 300, &g, &val) == -1); /* dy dominant */
    CHECK(p2_hit(&m, 1869, 90, 1780, 80, 300, &g, &val) == 1);    /* rescued */
    CHECK(g == P2_G_SWIPE_LEFT);

    /* slide region absorbs a tap; vertical fills upward */
    val = -1;
    CHECK(p2_hit(&m, 120, 1290, 120, 1290, 100, &g, &val) == 2);
    CHECK(g == P2_G_SLIDE);
    CHECK(val == 2);                     /* 10 px above bottom of 400 px */
    CHECK(p2_hit(&m, 120, 950, 120, 910, 600, &g, &val) == 2);
    CHECK(g == P2_G_SLIDE);
    CHECK(val == 97);                    /* lift near the top */
    /* lift dragged past the top: clamps to 100 */
    CHECK(p2_hit(&m, 120, 1000, 120, 700, 500, &g, &val) == 2);
    CHECK(val == 100);

    /* ---- gesture names ---- */
    CHECK(strcmp(p2_gesture_name(P2_G_TAP), "tap") == 0);
    CHECK(strcmp(p2_gesture_name(P2_G_SLIDE), "slide") == 0);
    CHECK(strcmp(p2_gesture_name(P2_G_SWIPE_DOWN), "swipe_down") == 0);

    /* ---- text blit on a synthetic framebuffer ---- */
    {
        /* 16x8 4bpp fb, mid-gray fill; atlas: 2 glyphs, strip 6px wide,
         * height 4: 'A' = solid black 4px, 'B' = solid white 2px. */
        enum { FW = 16, FH = 8 };
        static uint8_t fb[FW / 2 * FH];
        memset(fb, 0x77, sizeof fb);

        static uint8_t strip[3 * 4];   /* 6 px wide, 4 tall, 3 B/row */
        for (int r = 0; r < 4; r++) {
            strip[r * 3 + 0] = 0x00;   /* A: px 0-1 black */
            strip[r * 3 + 1] = 0x00;   /* A: px 2-3 black */
            strip[r * 3 + 2] = 0xFF;   /* B: px 4-5 white */
        }
        p2_atlas_t a;
        memset(&a, 0, sizeof a);
        a.height = 4; a.n_glyphs = 2;
        a.glyphs[0].ch = 'A'; a.glyphs[0].x = 0; a.glyphs[0].w = 4;
        a.glyphs[1].ch = 'B'; a.glyphs[1].x = 4; a.glyphs[1].w = 2;
        a.strip_w = 6; a.mean_w = 3; a.bits = strip;

        CHECK(p2_text_width(&a, "AB") == 6);
        CHECK(p2_text_width(&a, "A?B") == 9);   /* unknown = mean_w blank */

        p2_text_t t;
        memset(&t, 0, sizeof t);
        t.x = 2; t.y = 2; t.w = 12; t.h = 4; t.align = 0; t.max_chars = 8;

        p2_draw_text(fb, FW, FH, &t, &a, "AB");
        /* row 2, px 2-5 = A (black), px 6-7 = B (white), px 8-13 cleared
         * white, px 0-1 and 14-15 untouched (0x7). */
        CHECK((fb[2 * 8 + 1] & 0x0F) == 0x0);      /* px 3 black */
        CHECK((fb[2 * 8 + 2] >> 4) == 0x0);        /* px 4 black */
        CHECK((fb[2 * 8 + 3] >> 4) == 0xF);        /* px 6 white (B) */
        CHECK((fb[2 * 8 + 4] >> 4) == 0xF);        /* px 8 cleared */
        CHECK((fb[2 * 8 + 0] >> 4) == 0x7);        /* px 0 untouched */
        CHECK((fb[2 * 8 + 7] & 0x0F) == 0x7);      /* px 15 untouched */
        CHECK((fb[1 * 8 + 2] >> 4) == 0x7);        /* row above untouched */

        /* ODD pen phase: right-align "B" (2px) in the 12-wide rect ->
         * pen = 2+12-2 = 12 (even); use align center with "A" (4px):
         * pen = 2 + (12-4)/2 = 6 (even)... force odd via max_chars rect:
         * set rect x=3 (odd, per-pixel clear path) width 11. */
        memset(fb, 0x77, sizeof fb);
        t.x = 3; t.w = 11; t.align = 0;
        p2_draw_text(fb, FW, FH, &t, &a, "AB");
        CHECK((fb[2 * 8 + 1] & 0x0F) == 0x0);      /* px 3 = A black */
        CHECK((fb[2 * 8 + 3] & 0x0F) == 0xF);      /* px 7 = B white */
        CHECK((fb[2 * 8 + 1] >> 4) == 0x7);        /* px 2 untouched */

        /* no bits attached -> no-op */
        memset(fb, 0x77, sizeof fb);
        a.bits = NULL;
        p2_draw_text(fb, FW, FH, &t, &a, "AB");
        CHECK(fb[2 * 8 + 1] == 0x77);
    }

    /* ---- bundle parse ---- */
    {
        static const char BUNDLE[] =
        "{"
        "  \"bundle_digest\": \"9c01d2e4f6a81b3c\","
        "  \"states\": ["
        "    {\"kind\": \"frame\", \"state_id\": \"page:b2f1c3d4e5f60718\","
        "     \"frame_digest\": \"cd15a5fe1a4532e3\","
        "     \"manifest_digest\": \"77aa88bb99cc00dd\", \"bytes\": 1314144,"
        "     \"ttl_s\": 900, \"url\": \"/api/v1/device/d/bundle/frame/cd15a5fe1a4532e3\"},"
        "    {\"kind\": \"tile\", \"state_id\": \"st:tile_desk/on\","
        "     \"rect\": {\"x\": 128, \"y\": 640, \"w\": 300, \"h\": 90},"
        "     \"tile_digest\": \"e4b0aa11bb22cc33\", \"format\": \"fb-rect\","
        "     \"bytes\": 13500, \"url\": \"/api/v1/device/d/bundle/tile/e4b0aa11bb22cc33\"},"
        "    {\"kind\": \"tile\", \"state_id\": \"st:bad/size\","
        "     \"rect\": {\"x\": 128, \"y\": 640, \"w\": 300, \"h\": 90},"
        "     \"tile_digest\": \"a119bb22cc33dd44\", \"bytes\": 999,"
        "     \"url\": \"/x\"},"
        "    {\"kind\": \"hologram\", \"state_id\": \"st:future\","
        "     \"bytes\": 1, \"url\": \"/x\"},"
        "    {\"kind\": \"frame\", \"state_id\": \"page:wrongsize\","
        "     \"frame_digest\": \"cd15a5fe1a4532e4\", \"bytes\": 5,"
        "     \"url\": \"/x\"}"
        "  ],"
        "  \"links\": {\"page:home\": {\"swipe_left\": \"page:b2f1c3d4e5f60718\","
        "                              \"el:nav_next:tap\": \"page:b2f1c3d4e5f60718\"}}"
        "}";
        static p2_bundle_t b;
        CHECK(p2_bundle_parse(BUNDLE, sizeof BUNDLE - 1, 1314144, &b));
        CHECK(strcmp(b.bundle_digest, "9c01d2e4f6a81b3c") == 0);
        /* bad-size tile, unknown kind, wrong-size frame all dropped */
        CHECK(b.n_states == 2);
        CHECK(b.states[0].kind == P2_BK_FRAME && b.states[0].ttl_s == 900);
        CHECK(strcmp(b.states[0].man_digest, "77aa88bb99cc00dd") == 0);
        CHECK(b.states[1].kind == P2_BK_TILE);
        CHECK(b.states[1].bytes == 300u / 2 * 90);
        CHECK(b.n_links == 2);
        CHECK(p2_bundle_state(&b, "st:tile_desk/on") == &b.states[1]);
        CHECK(p2_bundle_state(&b, "st:nope") == NULL);
        const char *to = p2_bundle_link(&b, "page:home", "swipe_left");
        CHECK(to && strcmp(to, "page:b2f1c3d4e5f60718") == 0);
        CHECK(p2_bundle_link(&b, "page:home", "swipe_right") == NULL);
        CHECK(p2_bundle_link(&b, "page:away", "swipe_left") == NULL);
        /* strict: bad digest fails the whole document */
        CHECK(!p2_bundle_parse("{\"bundle_digest\": \"XYZ\"}", 24, 0, &b));
    }

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
