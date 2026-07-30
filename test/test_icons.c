/*
 * Host-side verification of the touch-v3 icon chain: the generated Phosphor
 * codepoint map, the vendored font binary, and the stb_truetype rasterizer,
 * exercised together against the REAL src/fonts/Phosphor-Bold.ttf.
 *
 * What this protects (none of it is caught by compiling):
 *   - the codepoint map is sorted, so the firmware's bsearch works at all;
 *   - every name in the map resolves to a glyph that EXISTS in the bundled
 *     font -- i.e. the map and the font are the same Phosphor release. A map
 *     regenerated from a different version is the exact glyph-mismatch failure
 *     the contract's version pin exists to prevent, and it would otherwise
 *     surface only as wrong pictures on a panel;
 *   - icons rasterize to sane, non-empty bitmaps at the sizes specs use;
 *   - an unknown name resolves to 0 rather than some arbitrary glyph.
 *
 * Build + run: tools/test_icons.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#include "phosphor_codepoints.h"

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#ifndef FONT_PATH
#define FONT_PATH "src/fonts/Phosphor-Bold.ttf"
#endif

static int cmp_entry(const void *key, const void *el)
{
    return strcmp((const char *)key, ((const t3_phosphor_entry_t *)el)->name);
}

static uint32_t cp_of(const char *name)
{
    const t3_phosphor_entry_t *e =
        bsearch(name, T3_PHOSPHOR_MAP, T3_PHOSPHOR_MAP_LEN,
                sizeof T3_PHOSPHOR_MAP[0], cmp_entry);
    return e ? e->cp : 0;
}

int main(void)
{
    printf("Phosphor %s %s, %d icons\n", T3_PHOSPHOR_VERSION,
           T3_PHOSPHOR_WEIGHT, T3_PHOSPHOR_MAP_LEN);

    /* The firmware bsearches this table, which is silently wrong if the
     * generator ever emits it unsorted. */
    int unsorted = 0;
    for (int i = 1; i < T3_PHOSPHOR_MAP_LEN; i++)
        if (strcmp(T3_PHOSPHOR_MAP[i - 1].name, T3_PHOSPHOR_MAP[i].name) >= 0)
            unsorted++;
    CHECK(unsorted == 0);
    CHECK(T3_PHOSPHOR_MAP_LEN > 1000);      /* a real map, not a stub */

    FILE *f = fopen(FONT_PATH, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", FONT_PATH); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    CHECK(buf && got == (size_t)n);
    if (!buf) return 1;

    stbtt_fontinfo fi;
    CHECK(stbtt_InitFont(&fi, buf, stbtt_GetFontOffsetForIndex(buf, 0)) != 0);

    /* THE pin check: every mapped name must exist in the bundled font. A map
     * from another release shows up here as missing glyphs. */
    int missing = 0;
    const char *first_missing = NULL;
    for (int i = 0; i < T3_PHOSPHOR_MAP_LEN; i++) {
        if (stbtt_FindGlyphIndex(&fi, (int)T3_PHOSPHOR_MAP[i].cp) == 0) {
            if (!first_missing) first_missing = T3_PHOSPHOR_MAP[i].name;
            missing++;
        }
    }
    if (missing)
        printf("  %d/%d mapped names missing from the font (first: %s) -- map "
               "and font are different Phosphor releases\n",
               missing, T3_PHOSPHOR_MAP_LEN, first_missing);
    CHECK(missing == 0);

    /* Rasterize at the sizes specs actually use. Em-box scaling matches the
     * server preview's CSS font-size; Phosphor can exceed the em by a pixel,
     * so allow px+1 (see touch3_icons.c). */
    static const char *NAMES[] = { "lightbulb", "film-slate", "fan", "house",
                                   "gear-six", "thermometer", "play", "power" };
    static const int SIZES[] = { 24, 32, 40, 64 };
    for (unsigned s = 0; s < sizeof SIZES / sizeof SIZES[0]; s++) {
        for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
            int px = SIZES[s];
            uint32_t cp = cp_of(NAMES[i]);
            CHECK(cp != 0);
            int gi = stbtt_FindGlyphIndex(&fi, (int)cp);
            CHECK(gi != 0);
            float sc = stbtt_ScaleForMappingEmToPixels(&fi, (float)px);
            int w = 0, h = 0, x0 = 0, y0 = 0;
            unsigned char *bm = stbtt_GetGlyphBitmap(&fi, sc, sc, gi,
                                                     &w, &h, &x0, &y0);
            CHECK(bm != NULL);
            if (!bm) continue;
            long ink = 0;
            for (int k = 0; k < w * h; k++) if (bm[k] > 127) ink++;
            CHECK(w > 0 && h > 0);
            CHECK(w <= px + 1 && h <= px + 1);     /* fits its reserved cell */
            CHECK(ink > 0);                        /* not a blank box */
            CHECK(ink < (long)w * h);              /* not a solid block */
            free(bm);
        }
    }

    /* An unknown name must resolve to 0 so the caller lays out without an
     * icon -- never to some arbitrary glyph. */
    CHECK(cp_of("definitely-not-an-icon") == 0);
    CHECK(cp_of("") == 0);

    free(buf);
    printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", tests, fails);
    return fails ? 1 : 0;
}
