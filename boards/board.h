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
#elif defined(TESSERAE_BOARD_SEEED_E1002)
#  include "seeed_reterminal_e1002.h"
#elif defined(TESSERAE_BOARD_SEEED_E1001)
#  include "seeed_reterminal_e1001.h"
#elif defined(TESSERAE_BOARD_SEEED_E1001_GRAY)
#  include "seeed_reterminal_e1001_gray.h"
#elif defined(TESSERAE_BOARD_SEEED_E1003)
#  include "seeed_reterminal_e1003.h"
#elif defined(TESSERAE_BOARD_WAVESHARE_PHOTOPAINTER_73)
#  include "waveshare_photopainter_73.h"
#elif defined(TESSERAE_BOARD_SEEED_EE02)
#  include "seeed_ee02.h"
#elif defined(TESSERAE_BOARD_XIAO_EPAPER_75)
#  include "seeed_xiao_epaper_75.h"
#elif defined(TESSERAE_BOARD_SEEED_EE04_75)
#  include "seeed_ee04_75.h"
#elif defined(TESSERAE_BOARD_SEEED_EE04_73E6)
#  include "seeed_ee04_73e6.h"
#else
#  error "No TESSERAE_BOARD_* selected. Set one in platformio.ini build_flags."
#endif
