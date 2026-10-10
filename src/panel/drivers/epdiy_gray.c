/*
 * Family G: epdiy-board parallel glass, 16-level grayscale (see the header).
 *
 * epdiy's high-level API keeps a front framebuffer (what we want on glass) and
 * a back framebuffer (what it believes is on glass) and drives only the
 * difference. Every full frame here starts with epd_fullclear(), which cycles
 * the panel black/white and leaves both buffers white, then a GC16 update of
 * the whole screen: the same "every update starts from a cleared panel" rule
 * the PaperS3 driver follows, so ghosting never builds up across frames and a
 * cold boot (both buffers white, glass showing the last frame) is no special
 * case.
 *
 * Partial refresh (display_partial) is epd_hl_update_area() over the changed
 * rect. It is compiled in only under -DEPD_EPDIY_PARTIAL, because providing it
 * makes the board advertise the overlay capability to the server
 * (overlay_run.c checks epd_supports_partial()), and that path has not been
 * seen on epdiy glass yet. The -partialtest env opts a bench unit in.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_EPDIY_GRAY)

#include "drivers/epdiy_gray.h"

#include <string.h>

/* epdiy's epd_init/epd_clear are compiled under these names (see
 * components_opt/epdiy_dep/CMakeLists.txt): the facade owns the plain ones. */
#define epd_init  epdiy_epd_init
#define epd_clear epdiy_epd_clear
#include "epdiy.h"
#include "esp_log.h"

static const char *TAG = "epd_epdiy";

#ifndef EPD_EPDIY_WAVEFORM
#define EPD_EPDIY_WAVEFORM EPD_BUILTIN_WAVEFORM   /* the display's own default */
#endif
#ifndef EPD_EPDIY_TEMP_FALLBACK_C
#define EPD_EPDIY_TEMP_FALLBACK_C 22
#endif

#define SRC_PITCH (EPD_WIDTH / 2)

static bool s_inited = false;
static EpdiyHighlevelState s_hl;

/* The panel's temperature picks the waveform's timing. The v7 board reads it
 * from the TPS65185's thermistor input; a board without one answers nonsense,
 * so anything outside the range e-paper is rated for falls back to room
 * temperature. Valid only while the panel is powered. */
static int panel_temp_c(void)
{
    const float t = epd_ambient_temperature();
    if (t < 0.0f || t > 50.0f) return EPD_EPDIY_TEMP_FALLBACK_C;
    return (int)(t + 0.5f);
}

/* Tesserae frame (high nibble = left) -> epdiy framebuffer (low nibble = left)
 * for rows y0..y0+h-1 and byte columns bx0..bx0+bw-1. */
static void copy_rows(const uint8_t *src, uint8_t *fb, int y0, int h, int bx0, int bw)
{
    for (int y = y0; y < y0 + h; y++) {
        const uint8_t *s = src + (size_t)y * SRC_PITCH + bx0;
        uint8_t *d = fb + (size_t)y * SRC_PITCH + bx0;
        for (int i = 0; i < bw; i++) {
            const uint8_t b = s[i];
            d[i] = (uint8_t)((b << 4) | (b >> 4));
        }
    }
}

static void report(const char *what, enum EpdDrawError err)
{
    if (err != EPD_DRAW_SUCCESS) ESP_LOGE(TAG, "%s failed: 0x%x", what, (unsigned)err);
}

/* ---------- driver entry points ---------- */

static esp_err_t ed_port_init(void)
{
    if (s_inited) return ESP_OK;
    epd_init(&EPD_EPDIY_BOARD, &EPD_EPDIY_DISPLAY, EPD_LUT_64K);
    if (epd_width() != EPD_WIDTH || epd_height() != EPD_HEIGHT) {
        ESP_LOGE(TAG, "epdiy display is %dx%d, board header says %dx%d",
                 epd_width(), epd_height(), EPD_WIDTH, EPD_HEIGHT);
        return ESP_ERR_INVALID_SIZE;
    }
    epd_set_vcom(EPD_EPDIY_VCOM_MV);
    s_hl = epd_hl_init(EPD_EPDIY_WAVEFORM);
    if (!s_hl.front_fb) {
        ESP_LOGE(TAG, "no PSRAM for the framebuffers");
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    ESP_LOGI(TAG, "epdiy up: %dx%d, VCOM -%d mV", EPD_WIDTH, EPD_HEIGHT, EPD_EPDIY_VCOM_MV);
    return ESP_OK;
}

/* Nothing to bring up per paint: power is taken around each update below. */
static void ed_init(void) { (void)ed_port_init(); }

static void ed_clear(uint8_t color)
{
    if (!s_inited) return;
    epd_poweron();
    const int t = panel_temp_c();
    epd_fullclear(&s_hl, t);
    if ((color & 0x0F) != 0x0F) {
        /* Anything but white: paint the level over the cleared panel. */
        const uint8_t packed = (uint8_t)(((color & 0x0F) << 4) | (color & 0x0F));
        memset(epd_hl_get_framebuffer(&s_hl), packed, (size_t)EPD_BUF_BYTES);
        report("clear", epd_hl_update_screen(&s_hl, MODE_GC16, t));
    }
    epd_poweroff();
}

static void ed_display(const uint8_t *image)
{
    if (!s_inited || !image) return;
    epd_poweron();
    const int t = panel_temp_c();
    epd_fullclear(&s_hl, t);
    copy_rows(image, epd_hl_get_framebuffer(&s_hl), 0, EPD_HEIGHT, 0, SRC_PITCH);
    report("update", epd_hl_update_screen(&s_hl, MODE_GC16, t));
    epd_poweroff();
    ESP_LOGI(TAG, "frame painted at %d C", t);
}

/* Diagnostic: the 16 grey levels as vertical bars, black at the left. Even
 * spacing between neighbours is the whole test; two bars collapsing into one
 * tone means the waveform does not separate those levels on this glass. */
static void ed_show_color_bars(void)
{
    if (!s_inited) return;
    epd_poweron();
    const int t = panel_temp_c();
    epd_fullclear(&s_hl, t);
    uint8_t *fb = epd_hl_get_framebuffer(&s_hl);
    const int bar_px = EPD_WIDTH / 16;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        uint8_t *row = fb + (size_t)y * SRC_PITCH;
        for (int bx = 0; bx < SRC_PITCH; bx++) {
            int level = (bx * 2) / bar_px;
            if (level > 15) level = 15;
            row[bx] = (uint8_t)((level << 4) | level);
        }
    }
    report("bars", epd_hl_update_screen(&s_hl, MODE_GC16, t));
    epd_poweroff();
    ESP_LOGI(TAG, "grey bars: levels 0..15 left to right at %d C", t);
}

static void ed_show_palette_sweep(void) { ed_show_color_bars(); }

static void ed_sleep(void)
{
    if (!s_inited) return;
    epd_poweroff();
}

#ifdef EPD_EPDIY_PARTIAL
/* Repaint one rect from the full frame. fast = DU (black/white only, quick);
 * otherwise GL16, the non-flashing grey waveform. The rect is widened to even
 * x so whole bytes copy; epdiy works out which pixels actually changed. */
static void ed_partial(const uint8_t *image, int x, int y, int w, int h, bool fast)
{
    if (!s_inited || !image) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_WIDTH) w = EPD_WIDTH - x;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    const int x0 = x & ~1;
    const int x1 = (x + w + 1) & ~1;
    copy_rows(image, epd_hl_get_framebuffer(&s_hl), y, h, x0 / 2, (x1 - x0) / 2);
    epd_poweron();
    const EpdRect area = { .x = x0, .y = y, .width = x1 - x0, .height = h };
    report("partial", epd_hl_update_area(&s_hl, fast ? MODE_DU : MODE_GL16, panel_temp_c(), area));
    epd_poweroff();
}
#endif

/* ---------- exported vtable ---------- */

const epd_driver_t epdiy_gray_driver = {
    .info = {
        .name      = EPD_EPDIY_PANEL_NAME,
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
        .grayscale = true,
    },
    .port_init          = ed_port_init,
    .init               = ed_init,
    .clear              = ed_clear,
    .display            = ed_display,
    .show_color_bars    = ed_show_color_bars,
    .show_palette_sweep = ed_show_palette_sweep,
    .sleep              = ed_sleep,
#ifdef EPD_EPDIY_PARTIAL
    .display_partial    = ed_partial,
#endif
};

#endif /* PANEL_DRIVER_EPDIY_GRAY */
