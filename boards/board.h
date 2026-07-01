/*
 * Board selector. platformio.ini defines exactly one -DTESSERAE_BOARD_*
 * per [env:...]; this header includes the matching board definition,
 * which supplies the pin map, panel geometry/palette, MCU tier, and the
 * PANEL_DRIVER_* macro that registry.c uses to bind the concrete driver.
 *
 * app_config.h includes this header, so every translation unit that pulls
 * in app_config.h sees the board's EPD_* macros -- exactly as before the
 * multi-board refactor, when those lived directly in app_config.h.
 */
#pragma once

#if defined(TESSERAE_BOARD_WAVESHARE_133E6)
#  include "waveshare_133e6.h"
#elif defined(TESSERAE_BOARD_SEEED_E1004)
#  include "seeed_reterminal_e1004.h"
#else
#  error "No TESSERAE_BOARD_* selected. Set one in platformio.ini build_flags."
#endif
