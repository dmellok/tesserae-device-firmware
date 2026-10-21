/*
 * Panel facade.
 *
 * This file used to hold the monolithic Waveshare 13.3E6 driver. That code
 * now lives in src/panel/drivers/spectra6_spi_dual.c behind the epd_driver_t
 * vtable; this facade preserves the original epd_driver.h API by forwarding
 * each call to the board's active driver (src/panel/registry.c). Callers
 * (main.c, splash.c, ...) are unchanged and panel-agnostic.
 */
#include "epd_driver.h"
#include "panel/epd_panel.h"

/* A dual-depth frame can be either length, and the overlay and touch engines
 * copy and patch exactly EPD_BUF_BYTES. No dual-depth board has either today;
 * one that gains them needs those buffers sized to the frame first. */
#if defined(EPD_GRAY_BUF_BYTES) && \
    ((defined(BOARD_OVERLAY_PARTIAL) && BOARD_OVERLAY_PARTIAL) || \
     (defined(BOARD_HAS_TOUCH) && BOARD_HAS_TOUCH))
#error "EPD_DUAL_DEPTH is not yet supported alongside overlay or touch"
#endif

esp_err_t epd_port_init(void)          { return epd_active_driver()->port_init(); }
void epd_init(void)                    { epd_active_driver()->init(); }
void epd_clear(uint8_t color)          { epd_active_driver()->clear(color); }
void epd_display(const uint8_t *image) { epd_active_driver()->display(image); }
void epd_show_color_bars(void)         { epd_active_driver()->show_color_bars(); }
void epd_show_palette_sweep(void)      { epd_active_driver()->show_palette_sweep(); }
void epd_sleep(void)                   { epd_active_driver()->sleep(); }

bool epd_supports_gray(void)
{
#ifdef EPD_GRAY_BUF_BYTES
    return epd_active_driver()->display_gray != NULL;
#else
    return false;
#endif
}

bool epd_frame_bytes_ok(size_t n)
{
    if (n == (size_t)EPD_BUF_BYTES) return true;
#ifdef EPD_GRAY_BUF_BYTES
    if (n == (size_t)EPD_GRAY_BUF_BYTES && epd_supports_gray()) return true;
#endif
    return false;
}

void epd_display_frame(const uint8_t *image, size_t n)
{
#ifdef EPD_GRAY_BUF_BYTES
    const epd_driver_t *d = epd_active_driver();
    if (n == (size_t)EPD_GRAY_BUF_BYTES && d->display_gray) {
        d->display_gray(image);
        return;
    }
#else
    (void)n;
#endif
    epd_display(image);
}

/* Partial refresh (overlay feature). Supported only where the active driver
 * provides display_partial; callers must check epd_supports_partial(). */
bool epd_supports_partial(void)
{
    return epd_active_driver()->display_partial != NULL;
}

void epd_display_partial(const uint8_t *image, int x, int y, int w, int h,
                         bool fast)
{
    const epd_driver_t *d = epd_active_driver();
    if (d->display_partial) d->display_partial(image, x, y, w, h, fast);
}

void epd_display_partial_mode(const uint8_t *image, int x, int y, int w, int h,
                              epd_refresh_t mode)
{
    const epd_driver_t *d = epd_active_driver();
    if (d->display_partial_mode) {
        d->display_partial_mode(image, x, y, w, h, mode);
    } else if (d->display_partial) {
        /* Driver has no waveform choice: A2 degrades to its fast path. */
        d->display_partial(image, x, y, w, h, mode != EPD_RF_GC16);
    }
}

bool epd_partial_is_rect_safe(epd_refresh_t mode)
{
    const epd_driver_t *d = epd_active_driver();
    return d->partial_rect_safe ? d->partial_rect_safe(mode) : true;
}

/* Row streaming (see epd_panel.h). All three are present or none is. */
bool epd_supports_stream(void)
{
    const epd_driver_t *d = epd_active_driver();
    return d->stream_begin && d->stream_rows && d->stream_end;
}

bool epd_stream_begin(void)
{
    const epd_driver_t *d = epd_active_driver();
    return d->stream_begin ? d->stream_begin() : false;
}

bool epd_stream_rows(const uint8_t *rows, int y0, int nrows)
{
    const epd_driver_t *d = epd_active_driver();
    return d->stream_rows ? d->stream_rows(rows, y0, nrows) : false;
}

bool epd_stream_rows_side(int side, const uint8_t *rows, int y0, int nrows)
{
    const epd_driver_t *d = epd_active_driver();
    if (d->stream_rows_side) return d->stream_rows_side(side, rows, y0, nrows);
    return d->stream_rows ? d->stream_rows(rows, y0, nrows) : false;
}

bool epd_stream_end(bool refresh)
{
    const epd_driver_t *d = epd_active_driver();
    return d->stream_end ? d->stream_end(refresh) : false;
}
