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

esp_err_t epd_port_init(void)          { return epd_active_driver()->port_init(); }
void epd_init(void)                    { epd_active_driver()->init(); }
void epd_clear(uint8_t color)          { epd_active_driver()->clear(color); }
void epd_display(const uint8_t *image) { epd_active_driver()->display(image); }
void epd_show_color_bars(void)         { epd_active_driver()->show_color_bars(); }
void epd_show_palette_sweep(void)      { epd_active_driver()->show_palette_sweep(); }
void epd_sleep(void)                   { epd_active_driver()->sleep(); }
