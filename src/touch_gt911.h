/*
 * touch_gt911.h -- minimal Goodix GT911 capacitive touch reader.
 *
 * Wake-from-deep-sleep only: no continuous interactive polling, no gesture
 * classification. The firmware captures a single raw stroke (start point, end
 * point, duration) translated to frame-pixel space and hands it to the server,
 * which does the tap/swipe/slide interpretation.
 *
 * All hardware entry points are compiled only when the active board defines
 * BOARD_HAS_TOUCH (currently just the reTerminal E1003); the other boards build
 * byte-identical. The pure coordinate math lives in touch_coords.[ch].
 */
#pragma once

#include "app_config.h"

#include <stdbool.h>
#include <stdint.h>

/* One captured stroke, already translated to the served frame's pixel space. */
typedef struct {
    bool     valid;   /* false => no point was readable (quick-tap race)        */
    int      x0, y0;  /* stroke start (frame pixels)                            */
    int      x1, y1;  /* stroke end   (frame pixels); == start for a point tap  */
    uint32_t ms;      /* time the finger was down, milliseconds                 */
} touch_stroke_t;

#ifdef BOARD_HAS_TOUCH

#include "esp_err.h"

/* Timing budget for a wake-triggered capture. */
#define TOUCH_CAP_MS          1500   /* max time to keep sampling a live stroke */
#define TOUCH_FIRST_POINT_MS   200   /* if no point by now, treat as quick tap  */
#define TOUCH_POLL_MS            8    /* inter-sample delay while a finger is down */

/* Bring up the GT911: release any deep-sleep hold on TP_RST, run the reset /
 * 0x5d address-select sequence, and probe the product id. ESP_OK on success;
 * on failure the caller continues as if touch were disabled (never stalls the
 * wake cycle). Idempotent within a boot. */
esp_err_t touch_init(void);

/* The probed product id (e.g. 0x00313931 == "911\0"), 0 until touch_init runs.
 * Exposed for the selftest. */
uint32_t touch_product_id(void);

/* Read one raw point. On ESP_OK, *pressed says whether a finger is down and, if
 * so, *rx/*ry hold the GT911 raw coordinate. Clears the status register so the
 * controller refills it. */
esp_err_t touch_read_raw(int *rx, int *ry, bool *pressed);

/* Read one point translated to frame-pixel space (touch_coords + the board's
 * orientation flags and the controller's configured output max). */
esp_err_t touch_read_frame(int *fx, int *fy, bool *pressed);

/* Translate an already-read raw point to frame-pixel space using the probed
 * output maxima and the board orientation flags. Lets a caller read raw once
 * (which clears the status register) and translate without re-reading. */
void touch_translate_raw(int rx, int ry, int *fx, int *fy);

/* Capture a full stroke after an EXT0 wake: wait up to first_point_ms for the
 * first readable point, then sample until finger lift or cap_ms, recording the
 * first and last points and the duration. Sliders depend on the END point, so
 * sampling continues while the finger stays down. Always fills *out (out->valid
 * distinguishes a real stroke from the quick-tap race). Returns ESP_OK. */
esp_err_t touch_capture_stroke(touch_stroke_t *out,
                               uint32_t first_point_ms, uint32_t cap_ms);

/* True if the INT line is currently asserted (a touch is waiting). Cheap GPIO
 * read used to poll during the linger window without hammering I2C. */
bool touch_int_asserted(void);

/* Prepare for deep sleep: leave the GT911 scanning (monitor mode) so it raises
 * INT on a touch, latch TP_RST with gpio_hold so the digital-domain pin does
 * not float and reset the controller (which would re-sample its I2C address),
 * and arm EXT0 wake on TP_INT going high. Call on the real deep-sleep path. */
void touch_prepare_sleep(void);

#endif /* BOARD_HAS_TOUCH */
