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
} epd_panel_info_t;

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
} epd_driver_t;

/* The single driver selected for this board at build time. Never NULL --
 * registry.c #errors at compile time if no driver is selected. */
const epd_driver_t *epd_active_driver(void);
