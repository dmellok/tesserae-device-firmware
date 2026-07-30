/*
 * Panel-driver abstraction.
 *
 * Every e-paper target implements this vtable in src/panel/drivers/. The
 * build selects exactly one concrete driver per board (registry.c, keyed
 * off the board's PANEL_DRIVER_* macro) and exposes it via
 * epd_active_driver(). The rest of the firmware talks to the panel only
 * through the thin epd_driver.h facade, which forwards to this vtable --
 * so adding a new panel family is a new drivers/*.c + a board header, with
 * no changes to main/splash/image_decoder.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Static description of a panel, filled in by each concrete driver. */
typedef struct {
    const char *name;     /* human-readable, e.g. "Spectra-6 13.3\" dual" */
    uint16_t    width;    /* pixels, native orientation */
    uint16_t    height;
    uint8_t     bpp;      /* bits per pixel of the panel-native frame */
    size_t      buf_bytes;/* full-frame buffer size the driver expects */
    /* True only when the nibbles are GRAY LEVELS the panel can render as
     * intermediate tones. bpp alone cannot answer this: the Spectra drivers are
     * also 4bpp, but their nibbles are palette indices into 6 fixed colours, and
     * a 1bpp mono panel has no intermediate level at all. Advertised to the
     * server as panel.grayscale, where it gates duotone icons (which need a
     * second ink level) -- see net_rest.c and the touch-v3 contract §5. */
    bool        grayscale;
} epd_panel_info_t;

/* Partial-refresh waveform, fastest to best. All e-paper waveforms trade
 * speed against fidelity and ghosting; naming the intent here lets a caller
 * pick rather than inferring it from a bool.
 *
 *   A2    2-level, fastest, most ghosting. Rejected for touch-v3 feedback on
 *         the E1003: fast but visibly filthy, and it could not drive an
 *         inverted rect back to white.
 *   DU    2-level, fast -- the long-standing overlay/proto2 echo waveform.
 *   GRAY  grayscale, fast, low ghosting -- the interactive default. The only
 *         tier that renders INTERMEDIATE levels in a partial refresh, which
 *         matters because primitives.json fills a switch's ON track and a
 *         slider's active span with `mid`: under any 2-level waveform that
 *         collapses to solid black and swallows the thumb drawn on top.
 *   GC16  16 levels, slowest -- full-quality frames and hygiene passes.
 *
 * A2 and DU are BOTH 2-level, so choosing A2 over DU costs ghosting and buys
 * nothing in gray depth. Ordered by speed; nothing persists these values. */
typedef enum {
    EPD_RF_A2 = 0,
    EPD_RF_DU,
    EPD_RF_GRAY,
    EPD_RF_GC16,
} epd_refresh_t;

/* Concrete driver vtable. Semantics mirror the original epd_driver.h API
 * one-to-one so the facade is a pure forward. */
typedef struct epd_driver {
    epd_panel_info_t info;

    /* One-time SPI bus + GPIO setup. Idempotent. */
    esp_err_t (*port_init)(void);
    /* Full panel power-up + reset + init sequence. */
    void (*init)(void);
    /* Fill the entire panel with a single palette color. */
    void (*clear)(uint8_t color);
    /* Push a full-frame buffer (info.buf_bytes long, panel-native layout). */
    void (*display)(const uint8_t *image);
    /* Diagnostic: paint the palette as horizontal color bands. */
    void (*show_color_bars)(void);
    /* Diagnostic: paint every raw nibble value as a band. */
    void (*show_palette_sweep)(void);
    /* Deep-sleep the panel and drop its power rail. */
    void (*sleep)(void);

    /* OPTIONAL (NULL = unsupported): repaint only the rect x,y,w,h from the
     * full framebuffer `image` (info.buf_bytes, panel-native layout; frame
     * coordinates -- the driver owns any panel-side mirror). fast = a quick
     * low-fidelity waveform (DU on IT8951) for overlay echo; !fast = full
     * quality (GC16) for hygiene passes. The overlay feature (overlay.h)
     * advertises itself only on boards whose driver provides this. */
    void (*display_partial)(const uint8_t *image, int x, int y, int w, int h,
                            bool fast);

    /* OPTIONAL (NULL = fall back to display_partial): the same repaint with an
     * explicit waveform, for callers that care which trade-off they take.
     * Added for touch v3, whose feedback path is latency-bound: DU measured
     * ~1.07-1.17 s per rect on the E1003, which is 2x its contract target, and
     * shrinking the rect does not help (the cost is the waveform, not the
     * pixels). Existing overlay/proto2 callers keep using display_partial and
     * are unaffected. */
    void (*display_partial_mode)(const uint8_t *image, int x, int y, int w,
                                 int h, epd_refresh_t mode);
} epd_driver_t;

/* The single driver selected for this board at build time. Never NULL --
 * registry.c #errors at compile time if no driver is selected. */
const epd_driver_t *epd_active_driver(void);
