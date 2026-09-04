/*
 * Host-side unit tests for touch3.c: frame-spec parse (all four primitives,
 * atlas dedupe, drop/skip rules, caps), gesture classification, hit-test
 * order, snap + slider axis math, stepper zones, value formatting with
 * suffix/max_chars, and the draw ops (ink inversion, rect/circle extents,
 * atlas text blit) on a synthetic 4bpp framebuffer. Build + run:
 *
 *   tools/test_touch3.sh
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "touch3.h"

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define PW 1872
#define PH 1404

/* The contract's example spec (firmware-build-prompt §7) plus edge cases. */
static const char SPEC[] =
"{"
" \"layout_digest\": \"8603c4372c14\","
" \"atlases\": ["
"   {\"id\":\"l20\",\"digest\":\"aa11bb22cc33dd44\",\"url\":\"/a/aa11\","
"    \"format\":\"gray4\",\"px\":20,\"weight\":400,\"strip_h\":24,"
"    \"ascent\":16,\"descent\":4,\"space_adv\":6,\"strip_w\":64,"
"    \"glyphs\":{\"D\":{\"x\":0,\"w\":14,\"adv\":15},"
"               \"e\":{\"x\":14,\"w\":12,\"adv\":13},"
"               \"s\":{\"x\":26,\"w\":10,\"adv\":11},"
"               \"k\":{\"x\":36,\"w\":12,\"adv\":13},"
"               \" \":{\"x\":0,\"w\":0,\"adv\":6},"
/* A stray "@name" key must be skipped: the finalized atlas is TEXT ONLY. */
"               \"@film-slate\":{\"x\":48,\"w\":16,\"adv\":16}}},"
"   {\"id\":\"v28\",\"digest\":\"ee55ff66aa77bb88\",\"url\":\"/a/ee55\","
"    \"format\":\"gray4\",\"px\":28,\"weight\":700,\"strip_h\":32,"
"    \"ascent\":22,\"descent\":6,\"space_adv\":8,\"strip_w\":40,"
"    \"glyphs\":{\"6\":{\"x\":0,\"w\":18,\"adv\":20},"
"               \"0\":{\"x\":18,\"w\":18,\"adv\":20},"
"               \"%\":{\"x\":36,\"w\":4,\"adv\":6}}},"
"   {\"id\":\"dup\",\"digest\":\"aa11bb22cc33dd44\",\"url\":\"/a/aa11\","
"    \"format\":\"gray4\",\"px\":20,\"weight\":400,\"strip_h\":24,"
"    \"ascent\":16,\"descent\":4,\"glyphs\":{\"D\":{\"x\":0,\"w\":14,\"adv\":15}}}"
" ],"
" \"primitives\": ["
"   {\"id\":\"btn_scene\",\"type\":\"button\","
"    \"rect\":{\"x\":40,\"y\":900,\"w\":180,\"h\":80},"
"    \"label\":{\"atlas\":\"l20\",\"align\":\"center\",\"text\":\"Desk\"},"
"    \"icon\":{\"name\":\"film-slate\",\"weight\":\"duotone\",\"px\":40},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"sw_desk\",\"type\":\"switch\","
"    \"rect\":{\"x\":240,\"y\":900,\"w\":160,\"h\":80},"
"    \"label\":{\"atlas\":\"l20\",\"text\":\"Desk\"},"
"    \"value_key\":\"ha:light.desk\",\"state\":\"on\","
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"sl_bri\",\"type\":\"slider\","
"    \"rect\":{\"x\":40,\"y\":1000,\"w\":360,\"h\":70},"
"    \"axis\":\"x\",\"min\":0,\"max\":100,\"step\":5,\"value\":60,"
"    \"value_key\":\"ha:light.desk:attributes.brightness_pct\","
"    \"value_text\":{\"atlas\":\"v28\",\"align\":\"center\",\"suffix\":\"%\","
"                   \"max_chars\":3},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"st_vol\",\"type\":\"stepper\","
"    \"rect\":{\"x\":420,\"y\":1000,\"w\":160,\"h\":70},"
"    \"min\":0,\"max\":30,\"step\":1,\"value\":12,"
"    \"value_text\":{\"atlas\":\"v28\",\"align\":\"center\",\"max_chars\":2},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
/* --- edge cases from here: each must DROP or DOWNGRADE, never fail --- */
"   {\"id\":\"dial_x\",\"type\":\"dial\","          /* unknown type: skip */
"    \"rect\":{\"x\":0,\"y\":0,\"w\":50,\"h\":50},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"no_action\",\"type\":\"button\","      /* missing action: drop */
"    \"rect\":{\"x\":600,\"y\":0,\"w\":50,\"h\":50}},"
"   {\"id\":\"off_panel\",\"type\":\"button\","      /* off-panel: drop */
"    \"rect\":{\"x\":5000,\"y\":10,\"w\":50,\"h\":50},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"overhang\",\"type\":\"button\","       /* clamped, kept */
"    \"rect\":{\"x\":1800,\"y\":10,\"w\":500,\"h\":50},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"sw_nokey\",\"type\":\"switch\","       /* no value_key: drop */
"    \"rect\":{\"x\":700,\"y\":0,\"w\":50,\"h\":50},"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"sl_norange\",\"type\":\"slider\","     /* max <= min: drop */
"    \"rect\":{\"x\":800,\"y\":0,\"w\":50,\"h\":50},"
"    \"axis\":\"x\",\"min\":10,\"max\":10,"
"    \"action\":{\"tier\":1,\"type\":\"ha\"}},"
"   {\"id\":\"btn_future\",\"type\":\"button\","     /* unknown action type */
"    \"rect\":{\"x\":900,\"y\":0,\"w\":50,\"h\":50},"
"    \"action\":{\"tier\":0,\"type\":\"teleport\"}}"
" ]"
"}";

static void test_parse(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    CHECK(strcmp(s.layout_digest, "8603c4372c14") == 0);
    CHECK(t3_spec_matches(&s, "8603c4372c14"));
    CHECK(!t3_spec_matches(&s, "deadbeef"));

    /* "dup" repeats l20's digest -> deduped away. */
    CHECK(s.n_atlases == 2);
    CHECK(strcmp(s.atlases[0].id, "l20") == 0);
    CHECK(s.atlases[0].strip_h == 24);
    CHECK(s.atlases[0].ascent == 16);
    CHECK(s.atlases[0].space_adv == 6);
    CHECK(s.atlases[1].px == 28 && s.atlases[1].weight == 700);
    /* Widest digit advance, for stable value boxes (v28 digits are adv 20). */
    CHECK(s.atlases[1].digit_w == 20);
    /* strip_w derived from max(x+w) when the descriptor omits it. */
    CHECK(s.atlases[1].strip_w == 40);

    /* 4 good + "no_action" + "overhang" + "btn_future" survive; 4 edge cases
     * drop/skip. A MISSING action is no longer a drop -- the live server omits
     * it on value-bound controls, so it defaults instead (see test_live_spec). */
    CHECK(s.n_prims == 7);
    CHECK(strcmp(s.prims[0].id, "btn_scene") == 0);
    CHECK(strcmp(s.prims[4].id, "no_action") == 0);
    CHECK(strcmp(s.prims[5].id, "overhang") == 0);
    CHECK(strcmp(s.prims[6].id, "btn_future") == 0);
    CHECK(t3_prim_by_id(&s, "dial_x") == NULL);
    const t3_prim_t *na = t3_prim_by_id(&s, "no_action");
    CHECK(na && na->tier == 1 && na->atype == T3_ACT_HA);
    CHECK(t3_prim_by_id(&s, "off_panel") == NULL);
    CHECK(t3_prim_by_id(&s, "sw_nokey") == NULL);
    CHECK(t3_prim_by_id(&s, "sl_norange") == NULL);

    /* Unknown action type downgrades to tier 2, keeps the control. */
    const t3_prim_t *fut = t3_prim_by_id(&s, "btn_future");
    CHECK(fut && fut->atype == T3_ACT_UNKNOWN && fut->tier == 2);

    /* Overhanging rect clamped to the panel. */
    const t3_prim_t *oh = t3_prim_by_id(&s, "overhang");
    CHECK(oh && oh->rect.x == 1800 && oh->rect.w == PW - 1800);

    const t3_prim_t *b = &s.prims[0];
    CHECK(b->type == T3_BUTTON && b->tier == 1 && b->atype == T3_ACT_HA);
    CHECK(b->label.present && b->label.atlas_idx == 0);
    CHECK(b->label.align == T3_ALIGN_CENTER);
    CHECK(strcmp(b->label.text, "Desk") == 0);
    CHECK(b->icon.present && b->icon.duotone && b->icon.px == 40);

    const t3_prim_t *sw = t3_prim_by_id(&s, "sw_desk");
    CHECK(sw && sw->type == T3_SWITCH && sw->state == true);
    CHECK(strcmp(sw->value_key, "ha:light.desk") == 0);
    /* A switch label defaults to left alignment (placement: left). */
    CHECK(sw->label.align == T3_ALIGN_LEFT);

    const t3_prim_t *sl = t3_prim_by_id(&s, "sl_bri");
    CHECK(sl && sl->axis == 'x');
    CHECK(sl->vmin == 0 && sl->vmax == 100 && sl->vstep == 5);
    CHECK(fabsf(sl->value - 60.0f) < 0.01f);
    CHECK(sl->value_text.present && sl->value_text.atlas_idx == 1);
    CHECK(strcmp(sl->value_text.suffix, "%") == 0);
    CHECK(sl->value_text.max_chars == 3);

    const t3_prim_t *st = t3_prim_by_id(&s, "st_vol");
    CHECK(st && st->type == T3_STEPPER && st->vstep == 1);
    CHECK(fabsf(st->value - 12.0f) < 0.01f);

    /* Strictness: no layout_digest, and a non-array primitives, both fail. */
    t3_spec_t bad;
    const char *no_anchor = "{\"primitives\":[]}";
    CHECK(!t3_spec_parse(no_anchor, strlen(no_anchor), PW, PH, &bad));
    const char *no_prims = "{\"layout_digest\":\"ab\"}";
    CHECK(!t3_spec_parse(no_prims, strlen(no_prims), PW, PH, &bad));
    CHECK(!t3_spec_parse("not json", 8, PW, PH, &bad));
    /* An empty primitives array is legal (a layout with no controls). */
    const char *empty = "{\"layout_digest\":\"ab\",\"primitives\":[]}";
    CHECK(t3_spec_parse(empty, strlen(empty), PW, PH, &bad));
    CHECK(bad.n_prims == 0);
}

static void test_gestures_and_hit(void)
{
    /* tap: displacement < 30 px AND duration < 500 ms. */
    CHECK(t3_classify(100, 100, 100, 100, 50) == T3_G_TAP);
    CHECK(t3_classify(100, 100, 120, 110, 300) == T3_G_TAP);   /* d ~= 22 */
    CHECK(t3_classify(100, 100, 140, 100, 300) == T3_G_DRAG);  /* d = 40 */
    CHECK(t3_classify(100, 100, 100, 100, 900) == T3_G_DRAG);  /* held */

    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    CHECK(t3_hit(&s, 50, 910) == 0);            /* btn_scene */
    CHECK(t3_hit(&s, 250, 910) == 1);           /* sw_desk */
    CHECK(t3_hit(&s, 40, 900) == 0);            /* inclusive top-left */
    CHECK(t3_hit(&s, 220, 900) == -1);          /* exclusive right edge */
    CHECK(t3_hit(&s, 40, 980) == -1);           /* exclusive bottom edge */
    CHECK(t3_hit(&s, 1000, 1300) == -1);        /* empty space */
}

static void test_numeric(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    t3_prim_t *sl = t3_prim_by_id(&s, "sl_bri");
    CHECK(sl != NULL);

    /* snap to the step grid, clamped to range. */
    CHECK(fabsf(t3_snap(sl, 62.0f) - 60.0f) < 0.01f);
    CHECK(fabsf(t3_snap(sl, 63.0f) - 65.0f) < 0.01f);
    CHECK(fabsf(t3_snap(sl, -5.0f) - 0.0f) < 0.01f);
    CHECK(fabsf(t3_snap(sl, 999.0f) - 100.0f) < 0.01f);

    /* track: inset 20 from each end. rect.x 40 w 360 -> origin 60, len 320. */
    int origin = 0, len = 0;
    t3_slider_track(sl, &origin, &len);
    CHECK(origin == 60 && len == 320);

    /* axis x: t = (fx - origin)/len, snapped. */
    CHECK(fabsf(t3_slider_value_at(sl, 60, 1030) - 0.0f) < 0.01f);
    CHECK(fabsf(t3_slider_value_at(sl, 380, 1030) - 100.0f) < 0.01f);
    CHECK(fabsf(t3_slider_value_at(sl, 220, 1030) - 50.0f) < 0.01f);
    CHECK(fabsf(t3_slider_value_at(sl, 10, 1030) - 0.0f) < 0.01f);   /* clamp */
    CHECK(fabsf(t3_slider_value_at(sl, 9999, 1030) - 100.0f) < 0.01f);

    /* axis y: the TOP of the track is max. */
    t3_prim_t vy = *sl;
    vy.axis = 'y';
    vy.rect.x = 40; vy.rect.y = 100; vy.rect.w = 70; vy.rect.h = 360;
    t3_slider_track(&vy, &origin, &len);
    CHECK(origin == 120 && len == 320);
    CHECK(fabsf(t3_slider_value_at(&vy, 50, 120) - 100.0f) < 0.01f);
    CHECK(fabsf(t3_slider_value_at(&vy, 50, 440) - 0.0f) < 0.01f);
    CHECK(fabsf(t3_slider_value_at(&vy, 50, 280) - 50.0f) < 0.01f);

    /* stepper thirds: rect.x 420 w 160 -> third 53. */
    t3_prim_t *st = t3_prim_by_id(&s, "st_vol");
    CHECK(st != NULL);
    CHECK(t3_stepper_zone(st, 430, 1010) == -1);
    CHECK(t3_stepper_zone(st, 500, 1010) == 0);
    CHECK(t3_stepper_zone(st, 570, 1010) == 1);
    CHECK(t3_stepper_zone(st, 100, 100) == -2);     /* outside */

    t3_rect_t mz = t3_stepper_zone_rect(st, -1);
    t3_rect_t pz = t3_stepper_zone_rect(st, 1);
    CHECK(mz.x == 420 && mz.w == 53);
    /* The plus zone absorbs the width remainder so the thirds tile exactly. */
    CHECK(pz.x == 420 + 106 && pz.x + pz.w == 420 + 160);

    /* switch track: label "Desk" = 15+13+11+13 = 52 wide, + gap 10. */
    t3_prim_t *sw = t3_prim_by_id(&s, "sw_desk");
    CHECK(sw != NULL);
    t3_rect_t tr = t3_switch_track_rect(sw, &s);
    CHECK(tr.x == 240 + 52 + 10);
    CHECK(tr.w == 160 - 62);
    CHECK(tr.h == 44);                              /* min(rect.h 80, 44) */
    CHECK(tr.y == 900 + (80 - 44) / 2);
}

static void test_format(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    t3_prim_t *sl = t3_prim_by_id(&s, "sl_bri");
    char buf[T3_VALTEXT_CAP];

    sl->value = 60;
    t3_format_value(sl, &sl->value_text, buf, sizeof buf);
    CHECK(strcmp(buf, "60%") == 0);

    sl->value = 100;                     /* "100%" clipped to max_chars 3 */
    t3_format_value(sl, &sl->value_text, buf, sizeof buf);
    CHECK(strcmp(buf, "100") == 0);

    /* Fractional step -> one decimal. */
    t3_prim_t f = *sl;
    f.vstep = 0.5f; f.value = 21.5f;
    f.value_text.suffix[0] = '\0';
    f.value_text.max_chars = 0;
    t3_format_value(&f, &f.value_text, buf, sizeof buf);
    CHECK(strcmp(buf, "21.5") == 0);

    /* The degree sign is 2 bytes but ONE character of footprint. */
    t3_prim_t d = *sl;
    d.vstep = 1.0f; d.value = 21.0f;
    snprintf(d.value_text.suffix, sizeof d.value_text.suffix, "\xC2\xB0");
    d.value_text.max_chars = 3;
    t3_format_value(&d, &d.value_text, buf, sizeof buf);
    CHECK(strcmp(buf, "21\xC2\xB0") == 0);
}

/* ---- draw ops on a synthetic 4bpp framebuffer ---- */

/* Wide enough that a 36 px slider thumb does not swallow its whole track. */
#define FW 256
#define FH 128
static uint8_t g_fb[(FW / 2) * FH];

static void fb_clear(uint8_t nibble)
{
    memset(g_fb, (uint8_t)((nibble << 4) | nibble), sizeof g_fb);
}

static uint8_t fb_at(int x, int y)
{
    uint8_t b = g_fb[(size_t)y * (FW / 2) + (x >> 1)];
    return (x & 1) ? (b & 0x0F) : (uint8_t)(b >> 4);
}

static void test_draw_ops(void)
{
    /* Ink inversion: ink 15 must land as panel nibble 0x0 (black), paper 0
     * as 0xF (white). This is the v3-vs-panel scale flip. */
    fb_clear(0x7);
    t3_fill_rect(g_fb, FW, FH, 4, 10, 10, 20, 10, T3_INK_INK);
    CHECK(fb_at(10, 10) == 0x0);
    CHECK(fb_at(29, 19) == 0x0);
    CHECK(fb_at(30, 10) == 0x7);            /* exclusive right edge */
    CHECK(fb_at(10, 20) == 0x7);            /* exclusive bottom edge */
    CHECK(fb_at(9, 10) == 0x7);

    t3_fill_rect(g_fb, FW, FH, 4, 10, 10, 20, 10, T3_INK_PAPER);
    CHECK(fb_at(10, 10) == 0xF);
    t3_fill_rect(g_fb, FW, FH, 4, 10, 10, 2, 1, T3_INK_MID);
    CHECK(fb_at(10, 10) == 0x7);            /* mid 8 -> 15-8 = 7 */
    t3_fill_rect(g_fb, FW, FH, 4, 10, 10, 2, 1, T3_INK_SOFT);
    CHECK(fb_at(10, 10) == 0xC);            /* soft 3 -> 12 */

    /* Odd x / odd w take the per-pixel path; check both nibble phases. */
    fb_clear(0xF);
    t3_fill_rect(g_fb, FW, FH, 4, 5, 5, 3, 1, T3_INK_INK);
    CHECK(fb_at(4, 5) == 0xF);
    CHECK(fb_at(5, 5) == 0x0 && fb_at(6, 5) == 0x0 && fb_at(7, 5) == 0x0);
    CHECK(fb_at(8, 5) == 0xF);

    /* Clipping: a rect straddling the edges must not corrupt memory. */
    fb_clear(0xF);
    t3_fill_rect(g_fb, FW, FH, 4, -10, -10, 20, 20, T3_INK_INK);
    CHECK(fb_at(0, 0) == 0x0 && fb_at(9, 9) == 0x0 && fb_at(10, 10) == 0xF);
    t3_fill_rect(g_fb, FW, FH, 4, FW - 5, FH - 5, 40, 40, T3_INK_INK);
    CHECK(fb_at(FW - 1, FH - 1) == 0x0);

    /* Rounded-rect outline: corners stay paper, edge midpoints are ink,
     * the interior is untouched. */
    fb_clear(0xF);
    t3_round_rect(g_fb, FW, FH, 4, 10, 10, 60, 40, 12, 2, T3_INK_INK);
    CHECK(fb_at(10, 10) == 0xF);            /* clipped corner */
    CHECK(fb_at(40, 10) == 0x0);            /* top edge */
    CHECK(fb_at(40, 11) == 0x0);            /* stroke is 2 px */
    CHECK(fb_at(40, 12) == 0xF);            /* and no thicker */
    CHECK(fb_at(40, 49) == 0x0);            /* bottom edge */
    CHECK(fb_at(10, 30) == 0x0);            /* left edge */
    CHECK(fb_at(69, 30) == 0x0);            /* right edge */
    CHECK(fb_at(40, 30) == 0xF);            /* hollow interior */

    /* Filled rounded rect: interior ink, corner clipped. */
    fb_clear(0xF);
    t3_fill_round_rect(g_fb, FW, FH, 4, 10, 10, 60, 40, 12, T3_INK_INK);
    CHECK(fb_at(40, 30) == 0x0);
    CHECK(fb_at(10, 10) == 0xF);
    CHECK(fb_at(40, 10) == 0x0);

    /* Circles: centre + cardinal points in, diagonal corner out. */
    fb_clear(0xF);
    t3_fill_circle(g_fb, FW, FH, 4, 50, 50, 20, T3_INK_INK);
    CHECK(fb_at(50, 50) == 0x0);
    CHECK(fb_at(50, 41) == 0x0);            /* r = 10 */
    CHECK(fb_at(50, 39) == 0xF);
    CHECK(fb_at(42, 42) == 0xF);            /* outside the arc (d^2 = 128) */

    fb_clear(0xF);
    t3_stroke_circle(g_fb, FW, FH, 4, 50, 50, 20, 2, T3_INK_INK);
    CHECK(fb_at(50, 40) == 0x0);            /* on the ring */
    CHECK(fb_at(50, 50) == 0xF);            /* hollow centre */

    /* 1bpp path: ink >= 8 goes black (bit clear), paper stays white. */
    uint8_t mono[(FW / 8) * FH];
    memset(mono, 0xFF, sizeof mono);
    t3_fill_rect(mono, FW, FH, 1, 8, 8, 8, 8, T3_INK_INK);
    CHECK(mono[(size_t)8 * (FW / 8) + 1] == 0x00);
    CHECK(mono[(size_t)8 * (FW / 8) + 0] == 0xFF);

    /* 2bpp path (4-gray panels): paper 3, soft 2, mid 1, ink 0, so every
     * level survives; the aligned span takes the byte path, an odd one the
     * per-pixel path. */
    uint8_t g2[(FW / 4) * FH];
    memset(g2, 0xFF, sizeof g2);
    t3_fill_rect(g2, FW, FH, 2, 8, 8, 8, 2, T3_INK_INK);     /* aligned */
    CHECK(g2[(size_t)8 * (FW / 4) + 2] == 0x00);
    CHECK(g2[(size_t)8 * (FW / 4) + 3] == 0x00);
    CHECK(g2[(size_t)8 * (FW / 4) + 1] == 0xFF);
    CHECK(g2[(size_t)10 * (FW / 4) + 2] == 0xFF);           /* exclusive bottom */
    t3_fill_rect(g2, FW, FH, 2, 8, 8, 4, 1, T3_INK_SOFT);
    CHECK(g2[(size_t)8 * (FW / 4) + 2] == 0xAA);            /* 2,2,2,2 */
    t3_fill_rect(g2, FW, FH, 2, 8, 8, 4, 1, T3_INK_MID);
    CHECK(g2[(size_t)8 * (FW / 4) + 2] == 0x55);            /* 1,1,1,1 */
    t3_fill_rect(g2, FW, FH, 2, 9, 12, 2, 1, T3_INK_INK);   /* odd: px 9,10 */
    CHECK(g2[(size_t)12 * (FW / 4) + 2] == 0xC3);           /* 11 00 00 11 */
}

/* An atlas whose strip is a solid ink ramp, so a blit is easy to assert. */
static void test_text_blit(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    t3_atlas_t *a = &s.atlases[0];          /* l20: strip_w 64, strip_h 24 */

    CHECK(t3_text_width(a, "Desk") == 15 + 13 + 11 + 13);
    CHECK(t3_text_width(a, "D k") == 15 + 6 + 13);      /* space_adv */
    CHECK(t3_text_width(a, "Dz") == 15 + a->mean_w);    /* unknown -> mean */
    CHECK(a->mean_w == (14 + 12 + 10 + 12 + 0 + 16) / 5);  /* w > 0 only */

    /* Blit with no bits attached must be a no-op, never a crash. */
    fb_clear(0xF);
    CHECK(t3_draw_text(g_fb, FW, FH, 4, a, "Desk", 0, 0, 100, 24,
                       T3_ALIGN_LEFT) == 0);
    CHECK(fb_at(0, 0) == 0xF);

    /* Attach a synthetic strip: every pixel full ink. */
    static uint8_t strip[(64 / 2) * 24];
    memset(strip, 0xFF, sizeof strip);     /* nibbles 0xF = full INK */
    a->bits = strip;

    fb_clear(0xF);
    int adv = t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 24,
                           T3_ALIGN_LEFT);
    CHECK(adv == 15);
    /* glyph D is x 0 w 14: columns 0..13 inked, 14 clear. Ink 15 -> 0x0. */
    CHECK(fb_at(0, 0) == 0x0);
    CHECK(fb_at(13, 23) == 0x0);
    CHECK(fb_at(14, 0) == 0xF);

    /* Right align inside a 100 px box: "D" (adv 15) starts at 85. */
    fb_clear(0xF);
    t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 24, T3_ALIGN_RIGHT);
    CHECK(fb_at(84, 0) == 0xF);
    CHECK(fb_at(85, 0) == 0x0);

    /* Centre align: (100 - 15)/2 = 42. */
    fb_clear(0xF);
    t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 24, T3_ALIGN_CENTER);
    CHECK(fb_at(41, 0) == 0xF);
    CHECK(fb_at(42, 0) == 0x0);

    /* Vertical centring uses ascent+descent (20) inside a taller box. */
    fb_clear(0xF);
    t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 40, T3_ALIGN_LEFT);
    CHECK(fb_at(0, 9) == 0xF);              /* top = (40-20)/2 = 10 */
    CHECK(fb_at(0, 10) == 0x0);

    /* Text is clipped to the box, never spilled past it. */
    fb_clear(0xF);
    t3_draw_text(g_fb, FW, FH, 4, a, "Desk", 0, 0, 20, 24, T3_ALIGN_LEFT);
    CHECK(fb_at(19, 0) == 0x0);
    CHECK(fb_at(20, 0) == 0xF);

    /* Paper pixels in a strip are transparent: they must not erase the
     * background the glyph sits on. */
    memset(strip, 0x00, sizeof strip);     /* all paper */
    fb_clear(0x7);
    t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 24, T3_ALIGN_LEFT);
    CHECK(fb_at(0, 0) == 0x7);
    a->bits = NULL;
}

/* Whole-primitive renders: assert the geometry lands where firmware-spec §7
 * says, using a spec placed inside the small test framebuffer. */
static const char SMALL[] =
"{\"layout_digest\":\"d1\",\"primitives\":["
" {\"id\":\"b\",\"type\":\"button\",\"rect\":{\"x\":4,\"y\":4,\"w\":60,\"h\":40},"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"w\",\"type\":\"switch\",\"rect\":{\"x\":4,\"y\":50,\"w\":60,\"h\":40},"
"  \"value_key\":\"ha:x\",\"state\":\"off\","
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"s\",\"type\":\"slider\",\"rect\":{\"x\":66,\"y\":4,\"w\":180,\"h\":40},"
"  \"axis\":\"x\",\"min\":0,\"max\":100,\"step\":10,\"value\":0,"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"p\",\"type\":\"stepper\",\"rect\":{\"x\":66,\"y\":50,\"w\":60,\"h\":40},"
"  \"min\":0,\"max\":10,\"step\":1,\"value\":5,"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}}"
"]}";

static void test_draw_primitives(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SMALL, strlen(SMALL), FW, FH, &s));
    CHECK(s.n_prims == 4);

    /* button: paper fill + rounded ink outline, hollow middle. */
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[0]);
    CHECK(fb_at(34, 5) == 0x0);             /* top edge (inset stroke/2 = 1) */
    CHECK(fb_at(34, 24) == 0xF);            /* interior is paper, not ink */
    CHECK(fb_at(3, 4) == 0x7);              /* nothing outside the rect */

    /* switch, off: thumb parked left inside the track. */
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[1]);
    t3_rect_t tr = t3_switch_track_rect(&s.prims[1], &s);
    CHECK(tr.h == 40);                      /* min(rect.h 40, 44) */
    CHECK(tr.x == 4 && tr.w == 60);         /* no drawable label */
    int d = tr.h - 2 * T3_SWITCH_INSET;     /* 32 */
    int cy = tr.y + tr.h / 2;
    int cx_off = tr.x + T3_SWITCH_INSET + d / 2;
    CHECK(fb_at(cx_off, cy) == 0x0);        /* thumb ink */
    int cx_on = tr.x + tr.w - T3_SWITCH_INSET - d / 2;
    CHECK(fb_at(cx_on, cy) == 0xF);         /* the ON end is empty */

    /* switch, on: track fills mid and the thumb moves right. */
    t3_prim_t on = s.prims[1];
    on.state = true;
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &on);
    CHECK(fb_at(cx_on, cy) == 0x0);
    CHECK(fb_at(cx_off, cy) == 0x7 || fb_at(cx_off, cy) == 0x0 ||
          fb_at(cx_off, cy) == 0xF);        /* either track fill or paper */
    /* The mid track fill is visible between the stroke and the thumb. */
    CHECK(fb_at(tr.x + tr.w / 2, tr.y + 2) == 0x7 ||
          fb_at(tr.x + tr.w / 2, tr.y + 3) == 0x7);

    /* slider at min: thumb centred on the track origin. */
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[2]);
    int origin = 0, len = 0;
    t3_slider_track(&s.prims[2], &origin, &len);
    CHECK(origin == 66 + 20 && len == 180 - 40);
    /* Thumb is paper-filled with an ink ring, so its centre reads paper. */
    int sty = 4 + (40 - T3_SLIDER_THICK) / 2;
    int scy = sty + T3_SLIDER_THICK / 2;
    CHECK(fb_at(origin, scy) == 0xF);
    CHECK(fb_at(origin, scy - T3_SLIDER_THUMB_D / 2) == 0x0);   /* ring */

    /* slider at max: the active fill runs the whole track. */
    t3_prim_t full = s.prims[2];
    full.value = 100;
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &full);
    CHECK(fb_at(origin + len / 2, scy) == 0x7);   /* mid ink 8 -> 0x7 */

    /* stepper: dividers at the thirds, marks in the outer thirds. */
    fb_clear(0x7);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[3]);
    int third = 60 / 3;                     /* 20 */
    CHECK(fb_at(66 + third, 50 + 20) == 0xC);          /* soft divider */
    CHECK(fb_at(66 + 2 * third, 50 + 20) == 0xC);
    CHECK(fb_at(66 + third / 2, 50 + 20) == 0x0);      /* minus bar */
    int pcx = 66 + 2 * third + (60 - 2 * third) / 2;
    CHECK(fb_at(pcx, 50 + 20) == 0x0);                 /* plus, horizontal */
    CHECK(fb_at(pcx, 50 + 20 - 2) == 0x0);             /* plus, vertical */

    /* feedback rects: whole rect for button/stepper, track for a switch. */
    t3_rect_t fb_b = t3_feedback_rect(&s, &s.prims[0]);
    CHECK(fb_b.x == 4 && fb_b.y == 4 && fb_b.w == 60 && fb_b.h == 40);
    t3_rect_t fb_w = t3_feedback_rect(&s, &s.prims[1]);
    CHECK(fb_w.h == tr.h && fb_w.y == tr.y);

    /* Every primitive must render without touching a byte outside the fb. */
    for (int i = 0; i < s.n_prims; i++) {
        fb_clear(0xF);
        t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[i]);
    }
    CHECK(strcmp(t3_ptype_name(T3_SLIDER), "slider") == 0);
    CHECK(strcmp(t3_interaction_name(T3_G_TAP), "tap") == 0);
    CHECK(strcmp(t3_interaction_name(T3_G_DRAG), "set") == 0);
}

/* Server sync 2026-07-27, DELTA 2: specs arrive with label/value_text refs but
 * NO atlases array until the atlas pipeline lands. Every primitive must still
 * render its chrome (geometry) and simply omit text -- "never a blank control".
 * Also covers the 16-hex layout_digest the live server returns. */
static const char NO_ATLAS[] =
"{\"layout_digest\":\"8603c4372c14c7b9\",\"primitives\":["
" {\"id\":\"b\",\"type\":\"button\",\"rect\":{\"x\":4,\"y\":4,\"w\":60,\"h\":40},"
"  \"label\":{\"atlas\":\"l20\",\"text\":\"Movie\"},"
"  \"icon\":{\"name\":\"film-slate\",\"px\":40},"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"w\",\"type\":\"switch\",\"rect\":{\"x\":4,\"y\":50,\"w\":60,\"h\":40},"
"  \"label\":{\"atlas\":\"l20\",\"text\":\"Desk\"},"
"  \"value_key\":\"ha:light.desk\",\"state\":\"on\","
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"s\",\"type\":\"slider\",\"rect\":{\"x\":66,\"y\":4,\"w\":180,\"h\":40},"
"  \"axis\":\"x\",\"min\":0,\"max\":100,\"step\":5,\"value\":50,"
"  \"value_text\":{\"atlas\":\"v28\",\"suffix\":\"%\",\"max_chars\":4},"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}},"
" {\"id\":\"p\",\"type\":\"stepper\",\"rect\":{\"x\":66,\"y\":50,\"w\":60,\"h\":40},"
"  \"min\":0,\"max\":30,\"step\":1,\"value\":12,"
"  \"value_text\":{\"atlas\":\"v28\",\"max_chars\":2},"
"  \"action\":{\"tier\":1,\"type\":\"ha\"}}"
"]}";

static void test_no_atlases(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(NO_ATLAS, strlen(NO_ATLAS), FW, FH, &s));
    CHECK(s.n_atlases == 0);
    CHECK(s.n_prims == 4);
    CHECK(strcmp(s.layout_digest, "8603c4372c14c7b9") == 0);

    /* Refs are present but unresolved: no atlas exists to point at. */
    CHECK(s.prims[0].label.present && s.prims[0].label.atlas_idx == -1);
    CHECK(s.prims[2].value_text.present &&
          s.prims[2].value_text.atlas_idx == -1);

    /* Chrome still renders for all four. */
    fb_clear(0x7);
    for (int i = 0; i < s.n_prims; i++)
        t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[i]);

    CHECK(fb_at(34, 5) == 0x0);              /* button frame top edge */
    CHECK(fb_at(34, 24) == 0xF);             /* interior paper, no text */

    /* With no label to measure, the switch track spans the whole rect -- it
     * must not reserve a phantom label gutter. */
    t3_rect_t tr = t3_switch_track_rect(&s.prims[1], &s);
    CHECK(tr.x == 4 && tr.w == 60);
    int d = tr.h - 2 * T3_SWITCH_INSET;
    int cx_on = tr.x + tr.w - T3_SWITCH_INSET - d / 2;
    CHECK(fb_at(cx_on, tr.y + tr.h / 2) == 0x0);   /* thumb, state on */

    /* Likewise the slider uses its full height (no value_text strip reserved)
     * and its feedback rect collapses to the track band. */
    t3_rect_t fr = t3_feedback_rect(&s, &s.prims[2]);
    CHECK(fr.y == 4 && fr.h == 40);
    int origin = 0, len = 0;
    t3_slider_track(&s.prims[2], &origin, &len);
    int scy = 4 + (40 - T3_SLIDER_THICK) / 2 + T3_SLIDER_THICK / 2;
    CHECK(fb_at(origin + 2, scy) == 0x7);    /* mid active fill at 50 % */

    /* Stepper keeps dividers + marks with no value drawn. */
    int third = 60 / 3;
    CHECK(fb_at(66 + third, 50 + 20) == 0xC);
    CHECK(fb_at(66 + third / 2, 50 + 20) == 0x0);

    /* Drawing text through an unresolved ref must be a safe no-op, not a
     * NULL-atlas crash. */
    CHECK(t3_draw_text(g_fb, FW, FH, 4, NULL, "x", 0, 0, 10, 10,
                       T3_ALIGN_LEFT) == 0);
}

/* Atlas is TEXT ONLY in the finalized contract, and each packed cell is
 * ADVANCE-wide (adv == w). Icons come from the bundled Phosphor font, which is
 * not present yet, so a button with an icon must lay out label-only rather than
 * reserving an empty icon box. */
static void test_text_only_atlas(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));

    /* "@film-slate" is not a charset key -> skipped, not stored anywhere. */
    t3_atlas_t *a = &s.atlases[0];
    int nb = 0;
    CHECK(t3_glyph(a, "@", &nb) == NULL);

    /* adv == w cells: glyphs abut exactly, so the string width is the sum of
     * the cell widths and each glyph lands at the running total. */
    t3_atlas_t adj;
    memset(&adj, 0, sizeof adj);
    adj.strip_w = 40; adj.strip_h = 8; adj.ascent = 8; adj.descent = 0;
    adj.space_adv = 10; adj.mean_w = 10; adj.digit_w = 10;
    for (char c = '0'; c <= '3'; c++) {
        int slot = c - 32;
        adj.have[slot] = true;
        adj.glyphs[slot].x   = (uint16_t)((c - '0') * 10);
        adj.glyphs[slot].w   = 10;      /* adv == w */
        adj.glyphs[slot].adv = 10;
    }
    CHECK(t3_text_width(&adj, "0123") == 40);

    static uint8_t strip[(40 / 2) * 8];
    memset(strip, 0xFF, sizeof strip);     /* every cell fully inked */
    adj.bits = strip;
    fb_clear(0xF);
    CHECK(t3_draw_text(g_fb, FW, FH, 4, &adj, "0123", 0, 0, 40, 8,
                       T3_ALIGN_LEFT) == 40);
    /* No gaps and no overlap: all 40 columns inked, column 40 clear. */
    bool solid = true;
    for (int x = 0; x < 40; x++) if (fb_at(x, 0) != 0x0) solid = false;
    CHECK(solid);
    CHECK(fb_at(40, 0) == 0xF);

    /* value box sizes off the widest digit + suffix, so it cannot clip "100%". */
    t3_prim_t *sl = t3_prim_by_id(&s, "sl_bri");
    CHECK(sl != NULL);
    int box = t3_value_box_w(&s.atlases[1], &sl->value_text);
    CHECK(box == 3 * 20 + 6);              /* max_chars 3 * digit 20 + '%' 6 */
    CHECK(box >= t3_text_width(&s.atlases[1], "100%"));

    /* No icon font bundled: report no icon so layout collapses to label-only. */
    CHECK(t3_icon_codepoint("film-slate") == 0);
    CHECK(t3_icon_width(&s.prims[0].icon) == 0);
    CHECK(t3_draw_icon(g_fb, FW, FH, 4, &s.prims[0].icon, 0, 0) == 0);
    adj.bits = NULL;
}

/* The touch_v3 experiment is DEFAULT OFF server-side, and the off state is an
 * EMPTY spec (valid, not an error). It must parse, hold zero controls, and leave
 * the panel dimensions alone. */
static void test_empty_spec(void)
{
    t3_spec_t s;
    const char *off = "{\"layout_digest\":\"8603c4372c14c7b9\","
                      "\"primitives\":[],\"atlases\":[]}";
    CHECK(t3_spec_parse(off, strlen(off), FW, FH, &s));
    CHECK(s.n_prims == 0);
    CHECK(s.n_atlases == 0);
    CHECK(strcmp(s.layout_digest, "8603c4372c14c7b9") == 0);
    CHECK(t3_hit(&s, 10, 10) == -1);
    CHECK(t3_prim_by_id(&s, "anything") == NULL);
}

/* Server sync DELTA 3: rects are FINAL device-framebuffer coordinates. The
 * engine must draw at the rect exactly as given and hit-test the same space --
 * no scaling, no rotation, no compensation anywhere. */
static void test_rects_are_final(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(NO_ATLAS, strlen(NO_ATLAS), FW, FH, &s));
    const t3_rect_t *r = &s.prims[0].rect;
    CHECK(r->x == 4 && r->y == 4 && r->w == 60 && r->h == 40);

    /* Hit-testing uses the same untransformed space as drawing: the drawn
     * frame's corners and the rect's bounds agree. */
    CHECK(t3_hit(&s, r->x, r->y) == 0);
    CHECK(t3_hit(&s, r->x + r->w - 1, r->y + r->h - 1) == 0);
    CHECK(t3_hit(&s, r->x - 1, r->y) != 0);
    CHECK(t3_hit(&s, r->x + r->w, r->y) != 0);

    fb_clear(0xF);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[0]);
    /* Ink appears inside the rect and nowhere outside it. */
    bool outside_clean = true;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            bool inside = x >= r->x && x < r->x + r->w &&
                          y >= r->y && y < r->y + r->h;
            if (!inside && fb_at(x, y) != 0xF) outside_clean = false;
        }
    CHECK(outside_clean);
}

/* ---- 1bpp (mono) treatment ----
 * Two inks force the four-level palette to fold. The folding direction is a
 * design decision (see touch3.c mono_white), and the failure it guards against
 * is a shape vanishing: an ink thumb on a mid track, or a soft divider on paper.
 * Chrome folds by level; glyph coverage thresholds at the midpoint instead. */
static uint8_t g_mono[(FW / 8) * FH];

static void mono_clear(void) { memset(g_mono, 0xFF, sizeof g_mono); }  /* white */

/* 1 = white, 0 = black (MSB = leftmost pixel). */
static int mono_at(int x, int y)
{
    return (g_mono[(size_t)y * (FW / 8) + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static void test_mono_palette(void)
{
    /* Chrome: only paper stays white. soft and mid MUST go black -- a divider
     * or a filled track that folds to paper disappears entirely. */
    struct { uint8_t ink; int white; const char *n; } cases[] = {
        { T3_INK_PAPER, 1, "paper" }, { T3_INK_SOFT,  0, "soft" },
        { T3_INK_MID,   0, "mid"   }, { T3_INK_INK,   0, "ink"  },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        mono_clear();
        t3_fill_rect(g_mono, FW, FH, 1, 8, 8, 8, 4, cases[i].ink);
        if (mono_at(8, 8) != cases[i].white) {
            tests++; fails++;
            printf("FAIL %s:%d: 1bpp %s folded wrong\n", __FILE__, __LINE__,
                   cases[i].n);
        } else tests++;
    }

    t3_spec_t s;
    CHECK(t3_spec_parse(SMALL, strlen(SMALL), FW, FH, &s));
    t3_prim_t *wp = &s.prims[1];              /* switch, state off */
    t3_rect_t tr = t3_switch_track_rect(wp, &s);
    int d = tr.h - 2 * T3_SWITCH_INSET;
    int cy = tr.y + tr.h / 2;
    int cx_off = tr.x + T3_SWITCH_INSET + d / 2;
    int cx_on  = tr.x + tr.w - T3_SWITCH_INSET - d / 2;

    /* OFF: paper track, ink thumb -> black thumb on white. */
    mono_clear();
    t3_draw_primitive(g_mono, FW, FH, 1, &s, wp);
    CHECK(mono_at(cx_off, cy) == 0);          /* thumb inked */
    CHECK(mono_at(cx_on, cy) == 1);           /* far end empty */

    /* ON: mid track folds to black, so the thumb flips to PAPER and stays
     * visible. This is the regression that mattered -- with an ink thumb the
     * whole track was one black blob and the state was unreadable. */
    wp->state = true;
    mono_clear();
    t3_draw_primitive(g_mono, FW, FH, 1, &s, wp);
    CHECK(mono_at(cx_on, cy) == 1);           /* white thumb */
    CHECK(mono_at(tr.x + 8, cy) == 0);        /* black track around it */
    /* ...and the two states must not render identically at the thumb. */
    CHECK(mono_at(cx_on, cy) != mono_at(cx_off, cy));

    /* Grayscale keeps ink-on-mid: the thumb is ink, not paper. */
    fb_clear(0xF);
    t3_draw_primitive(g_fb, FW, FH, 4, &s, wp);
    CHECK(fb_at(cx_on, cy) == 0x0);           /* ink thumb, 4bpp */

    /* Stepper dividers are soft -> must be visible (black) at 1bpp. */
    mono_clear();
    t3_draw_primitive(g_mono, FW, FH, 1, &s, &s.prims[3]);
    CHECK(mono_at(66 + 60 / 3, 50 + 20) == 0);
}

/* Glyph coverage is continuous tone, so 1bpp thresholds at the midpoint rather
 * than inking every non-paper level (which would fatten every glyph). */
static void test_mono_text_threshold(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(SPEC, strlen(SPEC), PW, PH, &s));
    t3_atlas_t *a = &s.atlases[0];            /* strip_w 64, strip_h 24 */
    static uint8_t strip[(64 / 2) * 24];
    a->bits = strip;

    /* Light coverage (ink 4): below the midpoint -> no ink at 1bpp... */
    memset(strip, 0x44, sizeof strip);
    mono_clear();
    t3_draw_text(g_mono, FW, FH, 1, a, "D", 0, 0, 100, 24, T3_ALIGN_LEFT);
    CHECK(mono_at(0, 0) == 1);
    /* ...while 4bpp still renders it as the light tone it is. */
    fb_clear(0xF);
    t3_draw_text(g_fb, FW, FH, 4, a, "D", 0, 0, 100, 24, T3_ALIGN_LEFT);
    CHECK(fb_at(0, 0) == 0xB);                /* 15 - 4 */

    /* Heavy coverage (ink 12): at or above the midpoint -> inks at 1bpp. */
    memset(strip, 0xCC, sizeof strip);
    mono_clear();
    t3_draw_text(g_mono, FW, FH, 1, a, "D", 0, 0, 100, 24, T3_ALIGN_LEFT);
    CHECK(mono_at(0, 0) == 0);
    a->bits = NULL;
}

/* Verbatim primitives captured from the LIVE server 2026-07-27, which exposed
 * two parser bugs that a schema-faithful test could never have caught:
 *
 *   1. Sliders and steppers arrive with NO "action" -- they act through their
 *      value_key. frame-spec.schema.json lists action as required, so requiring
 *      it silently dropped every slider and stepper on a real page.
 *   2. The page carried 46 primitives against a cap of 32, truncating 14.
 *
 * Reality beats the schema; keep this test anchored to the wire, not the doc. */
static const char LIVE_SPEC[] =
"{\"layout_digest\":\"ad7d67e655424e1c\",\"primitives\":["
"{\"type\":\"button\",\"id\":\"all_off\",\"rect\":{\"x\":840,\"y\":148,\"w\":150,\"h\":50},"
 "\"label\":{\"atlas\":\"l20\",\"align\":\"center\",\"text\":\"ALL OFF\"},"
 "\"icon\":{\"name\":\"power\",\"px\":40,\"weight\":\"bold\"},"
 "\"action\":{\"tier\":1,\"type\":\"ha\"}},"
"{\"type\":\"switch\",\"id\":\"sw_desk\",\"rect\":{\"x\":413,\"y\":468,\"w\":150,\"h\":66},"
 "\"value_key\":\"ha:light.desk\",\"action\":{\"tier\":1,\"type\":\"ha\"}},"
"{\"type\":\"slider\",\"id\":\"dim_desk\",\"rect\":{\"x\":42,\"y\":574,\"w\":519,\"h\":66},"
 "\"axis\":\"x\",\"min\":0,\"max\":100,\"step\":1,\"value\":0,"
 "\"value_key\":\"ha:light.desk\","
 "\"value_text\":{\"atlas\":\"v28\",\"align\":\"center\",\"max_chars\":4}},"
"{\"type\":\"stepper\",\"id\":\"ht_step\",\"rect\":{\"x\":1510,\"y\":334,\"w\":324,\"h\":68},"
 "\"min\":15,\"max\":28,\"step\":0.5,\"value\":15,"
 "\"value_key\":\"ha:climate.living_room_thermostat:attributes.temperature\","
 "\"value_text\":{\"atlas\":\"v28\",\"align\":\"center\",\"max_chars\":4}}"
"]}";

static void test_live_spec(void)
{
    t3_spec_t s;
    CHECK(t3_spec_parse(LIVE_SPEC, strlen(LIVE_SPEC), PW, PH, &s));
    CHECK(strcmp(s.layout_digest, "ad7d67e655424e1c") == 0);
    CHECK(s.n_atlases == 0);            /* server omits atlases (for now) */

    /* All four survive -- the actionless slider/stepper especially. */
    CHECK(s.n_prims == 4);
    const t3_prim_t *b  = t3_prim_by_id(&s, "all_off");
    const t3_prim_t *w  = t3_prim_by_id(&s, "sw_desk");
    const t3_prim_t *sl = t3_prim_by_id(&s, "dim_desk");
    const t3_prim_t *st = t3_prim_by_id(&s, "ht_step");
    CHECK(b && w && sl && st);
    if (!(b && w && sl && st)) return;

    /* Absent action defaults to tier 1 + ha: optimistic draw, report, let the
     * values stream confirm -- what a value-bound control wants. */
    CHECK(sl->tier == 1 && sl->atype == T3_ACT_HA);
    CHECK(st->tier == 1 && st->atype == T3_ACT_HA);
    /* An explicit action is still honoured. */
    CHECK(b->tier == 1 && b->atype == T3_ACT_HA);

    /* Rects are taken verbatim -- final framebuffer coords, no transform. */
    CHECK(sl->rect.x == 42 && sl->rect.y == 574 && sl->rect.w == 519);
    CHECK(st->rect.x == 1510 && st->rect.w == 324);
    CHECK(t3_hit(&s, 1510, 334) == t3_prim_by_id(&s, "ht_step") - s.prims);

    /* Fractional step survives and formats to one decimal. */
    CHECK(st->vstep == 0.5f && st->vmin == 15 && st->vmax == 28);
    char buf[T3_VALTEXT_CAP];
    t3_prim_t half = *st;
    half.value = 21.5f;
    t3_format_value(&half, &half.value_text, buf, sizeof buf);
    CHECK(strcmp(buf, "21.5") == 0);

    /* A long value_key must survive INTACT -- truncation parses fine and then
     * never matches the values stream, so the control stops reconciling. */
    CHECK(strcmp(st->value_key,
                 "ha:climate.living_room_thermostat:attributes.temperature") == 0);

    /* Text refs point at atlases that do not exist yet: unresolved, and the
     * chrome must still draw (no atlases => no text, never no control). */
    CHECK(sl->value_text.present && sl->value_text.atlas_idx == -1);
    CHECK(b->label.present && b->label.atlas_idx == -1);
    CHECK(b->icon.present && strcmp(b->icon.name, "power") == 0);

    fb_clear(0x7);
    for (int i = 0; i < s.n_prims; i++)
        t3_draw_primitive(g_fb, FW, FH, 4, &s, &s.prims[i]);   /* must not crash */
}

/* MAX_PRIMS / MAX_ATLASES caps must truncate, not overflow. */
static void test_caps(void)
{
    char big[24 * 1024];
    int n = snprintf(big, sizeof big, "{\"layout_digest\":\"cap\","
                     "\"primitives\":[");
    for (int i = 0; i < T3_MAX_PRIMS + 8; i++)
        n += snprintf(big + n, sizeof big - (size_t)n,
                      "%s{\"id\":\"b%d\",\"type\":\"button\","
                      "\"rect\":{\"x\":%d,\"y\":0,\"w\":10,\"h\":10},"
                      "\"action\":{\"tier\":1,\"type\":\"ha\"}}",
                      i ? "," : "", i, i * 12);
    n += snprintf(big + n, sizeof big - (size_t)n, "]}");

    t3_spec_t s;
    CHECK(t3_spec_parse(big, (size_t)n, PW, PH, &s));
    CHECK(s.n_prims == T3_MAX_PRIMS);
    char last[8];
    snprintf(last, sizeof last, "b%d", T3_MAX_PRIMS - 1);
    CHECK(strcmp(s.prims[T3_MAX_PRIMS - 1].id, last) == 0);
}

int main(void)
{
    test_parse();
    test_gestures_and_hit();
    test_numeric();
    test_format();
    test_draw_ops();
    test_text_blit();
    test_draw_primitives();
    test_live_spec();
    test_no_atlases();
    test_text_only_atlas();
    test_empty_spec();
    test_rects_are_final();
    test_mono_palette();
    test_mono_text_threshold();
    test_caps();
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", tests, fails);
    return fails ? 1 : 0;
}
