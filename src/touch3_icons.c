/*
 * touch3_icons.c -- Phosphor icon rasterizer for touch v3.
 *
 * The finalized contract renders button icons from a BUNDLED Phosphor weight:
 * the spec ships {"name":"film-slate","px":40} and the device resolves the name
 * to a codepoint, rasterizes it at that size, and blits the coverage as ink.
 * Icons never travel in the atlas (that is text-only).
 *
 * VERSION PIN (the contract's core guarantee is glyph-identity, not
 * pixel-identity, so both sides must resolve a name to the SAME glyph):
 *   - Font: Phosphor 2.1.2 bold, src/fonts/Phosphor-Bold.ttf, MIT
 *     (sha256 10a0a1cb4f8156a4...), embedded via EMBED_FILES.
 *   - Map:  include/phosphor_codepoints.h, generated from that release's
 *     stylesheet by tools/gen_phosphor_codepoints.py.
 *   - Verified against the server's own vendored copy: its Phosphor-Bold.woff2
 *     is BYTE-IDENTICAL to official 2.1.0/2.1.1/2.1.2 (same sha256), and the
 *     1530-entry codepoint map matches exactly with zero conflicts. The bold
 *     font and codepoints did not change across the 2.1.x line, so this pin is
 *     glyph-compatible with what the server already ships.
 *
 * Rasterizing costs ~(px*px) bytes of scratch for one glyph at a time and runs
 * only while composing a frame or repainting one control, so it stays off the
 * touch-feedback hot path.
 */

#include "touch3.h"

#ifdef T3_HAVE_ICON_FONT

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "phosphor_codepoints.h"

/* stb_truetype: rasterizer only, no rasterizer-internal allocations we cannot
 * see. STBTT_STATIC keeps its symbols out of the global namespace. */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_malloc(x, u) ((void)(u), malloc(x))
#define STBTT_free(x, u)   ((void)(u), free(x))
#include "vendor/stb_truetype.h"

static const char *TAG = "t3icon";

/* Embedded by the build (see src/CMakeLists.txt EMBED_FILES). */
extern const uint8_t phosphor_ttf_start[] asm("_binary_Phosphor_Bold_ttf_start");
extern const uint8_t phosphor_ttf_end[]   asm("_binary_Phosphor_Bold_ttf_end");

static stbtt_fontinfo s_font;
static bool s_ready, s_failed;

static bool font_ready(void)
{
    if (s_ready) return true;
    if (s_failed) return false;
    if (!stbtt_InitFont(&s_font, phosphor_ttf_start,
                        stbtt_GetFontOffsetForIndex(phosphor_ttf_start, 0))) {
        ESP_LOGE(TAG, "Phosphor %s failed to parse (%u bytes)",
                 T3_PHOSPHOR_VERSION,
                 (unsigned)(phosphor_ttf_end - phosphor_ttf_start));
        s_failed = true;                 /* latch: never retry per glyph */
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Phosphor %s %s ready (%u icons, %u KB)",
             T3_PHOSPHOR_VERSION, T3_PHOSPHOR_WEIGHT,
             (unsigned)T3_PHOSPHOR_MAP_LEN,
             (unsigned)((phosphor_ttf_end - phosphor_ttf_start) / 1024));
    return true;
}

static int cmp_entry(const void *key, const void *el)
{
    return strcmp((const char *)key, ((const t3_phosphor_entry_t *)el)->name);
}

uint32_t t3_icon_codepoint(const char *name)
{
    if (!name || !name[0]) return 0;
    const t3_phosphor_entry_t *e =
        bsearch(name, T3_PHOSPHOR_MAP, T3_PHOSPHOR_MAP_LEN,
                sizeof T3_PHOSPHOR_MAP[0], cmp_entry);
    return e ? e->cp : 0;
}

int t3_icon_width(const t3_icon_ref_t *icon)
{
    if (!icon || !icon->present || icon->px <= 0) return 0;
    if (!t3_icon_codepoint(icon->name)) return 0;
    if (!font_ready()) return 0;
    return icon->px;                     /* Phosphor cells are square */
}

int t3_draw_icon(uint8_t *fb, int fb_w, int fb_h, int bpp,
                 const t3_icon_ref_t *icon, int x, int y)
{
    if (!fb || !icon || !icon->present || icon->px <= 0) return 0;
    uint32_t cp = t3_icon_codepoint(icon->name);
    if (!cp) {
        ESP_LOGW(TAG, "icon '%s' not in Phosphor %s", icon->name,
                 T3_PHOSPHOR_VERSION);
        return 0;
    }
    if (!font_ready()) return 0;

    /* Map the EM BOX to px, not the ascent-descent span: the server's preview
     * renders the same icon as web-font text at font-size: <px>, and CSS
     * font-size is em-relative. stbtt_ScaleForPixelHeight would scale by
     * ascent+descent instead and draw a visibly different size.
     *
     * Phosphor glyphs can still rasterize a pixel over the em box (verified on
     * the real 2.1.2 font: a 40 px request yields 41 px for some icons), so the
     * inked bitmap is centred on the px cell and clipped rather than forced to
     * fit -- the contract guarantees glyph-identity, not pixel-identity, and a
     * 1 px difference at these sizes is invisible on e-ink. */
    float scale = stbtt_ScaleForMappingEmToPixels(&s_font, (float)icon->px);
    int gi = stbtt_FindGlyphIndex(&s_font, (int)cp);
    if (gi == 0) return 0;

    int gw = 0, gh = 0, gx0 = 0, gy0 = 0;
    unsigned char *cov = stbtt_GetGlyphBitmap(&s_font, scale, scale, gi,
                                              &gw, &gh, &gx0, &gy0);
    if (!cov) return 0;

    /* Centre the inked box inside the px cell: stb returns a tight bitmap, and
     * the glyph's own offsets are relative to the baseline, which we do not
     * want to expose to the layout (the caller reserved a square cell). */
    int ox = x + (icon->px - gw) / 2;
    int oy = y + (icon->px - gh) / 2;

    for (int r = 0; r < gh; r++) {
        int py = oy + r;
        if (py < 0 || py >= fb_h) continue;
        for (int c = 0; c < gw; c++) {
            int px_x = ox + c;
            if (px_x < 0 || px_x >= fb_w) continue;
            unsigned char a = cov[(size_t)r * gw + c];
            if (!a) continue;                       /* fully transparent */
            /* 8-bit coverage -> 4bpp ink level. The palette is ink-scale; the
             * draw op inverts for the panel. */
            uint8_t ink = (uint8_t)((a * T3_INK_INK + 127) / 255);
            if (!ink) continue;
            t3_fill_rect(fb, fb_w, fb_h, bpp, px_x, py, 1, 1, ink);
        }
    }
    stbtt_FreeBitmap(cov, NULL);
    return icon->px;
}

#endif /* T3_HAVE_ICON_FONT */
