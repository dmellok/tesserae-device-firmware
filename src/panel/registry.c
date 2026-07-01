/*
 * Build-time panel-driver selection.
 *
 * The active board header (pulled in via app_config.h -> boards/board.h)
 * defines exactly one PANEL_DRIVER_* macro. We bind epd_active_driver() to
 * the matching vtable here. Adding a family = one more #elif + drivers/*.c.
 */
#include "panel/epd_panel.h"
#include "app_config.h"   /* -> boards/board.h -> PANEL_DRIVER_* */

#if defined(PANEL_DRIVER_SPECTRA6_SPI_DUAL)
#  include "drivers/spectra6_spi_dual.h"
const epd_driver_t *epd_active_driver(void) { return &spectra6_spi_dual_driver; }
#elif defined(PANEL_DRIVER_SPECTRA6_T133A01_DUAL)
#  include "drivers/spectra6_t133a01_dual.h"
const epd_driver_t *epd_active_driver(void) { return &spectra6_t133a01_dual_driver; }
#elif defined(PANEL_DRIVER_SPECTRA6_SPI_SINGLE)
#  include "drivers/spectra6_spi_single.h"
const epd_driver_t *epd_active_driver(void) { return &spectra6_spi_single_driver; }
#else
#  error "No PANEL_DRIVER_* selected by the board header."
#endif
