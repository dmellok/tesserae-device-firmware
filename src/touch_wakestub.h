/*
 * touch_wakestub.h -- deep-sleep wake stub that captures a GT911 touch point
 * within ~1 ms of wake, before the ~1 s full boot lets the finger lift.
 *
 * The problem: a quick tap (~100 ms of contact) wakes the SoC via the TP_INT
 * ext1 line, but by the time app_main reaches touch_capture_stroke (~1 s later)
 * the finger is long gone and the GT911's point buffer has been overwritten by
 * the finger-up report (npoints=0). The tap is lost. This is the "1 out of 10"
 * miss rate seen on real E1003 hardware.
 *
 * The fix (mirrors how the TRMNL firmware handles its touch controller): run a
 * tiny routine out of RTC memory the instant the SoC wakes -- long before the
 * bootloader -- that hand-rolls an I2C read of the GT911 point buffer and stashes
 * the raw coordinate in RTC memory. app_main then prefers that stashed point over
 * a (likely empty) live read. Everything here must live in RTC memory: the code
 * is RTC_IRAM_ATTR, the stash is RTC_DATA_ATTR, and it may only touch ROM helpers
 * and raw registers (no flash, no drivers, no regular .data).
 *
 * Guarded by BOARD_TOUCH_WAKE_STUB (set in the board header). When that is not
 * defined this file compiles to nothing and app_main sees an always-empty stash.
 */
#pragma once

#include "app_config.h"

#include <stdint.h>
#include <stdbool.h>

/* How far the stub got on the last wake -- diagnostics readable from app_main. */
enum {
    TWS_STAGE_NOT_RUN     = 0,
    TWS_STAGE_PINS        = 1,   /* died configuring pins (should not happen) */
    TWS_STAGE_STATUS_FAIL = 2,   /* GT911 did not ACK the status read */
    TWS_STAGE_NO_POINT    = 3,   /* status read OK but no fresh point buffered */
    TWS_STAGE_POINT_FAIL  = 4,   /* point read NACKed mid-way */
    TWS_STAGE_CAPTURED    = 5,   /* rx/ry stashed */
};

typedef struct {
    uint32_t magic;    /* set to TOUCH_WAKE_MAGIC when rx/ry are valid */
    int32_t  rx;       /* raw GT911 X (pre-orientation) */
    int32_t  ry;       /* raw GT911 Y */
    uint32_t runs;     /* total stub executions since power-on (diag) */
    uint32_t stage;    /* TWS_STAGE_* reached on the last wake (diag) */
    uint32_t status;   /* last GT911 status byte the stub read (diag) */
} touch_wake_capture_t;

/* The stash the wake stub writes and app_main reads. Always defined (even without
 * the stub) so app_main can link unconditionally; without the stub it stays empty. */
extern touch_wake_capture_t g_touch_wake_capture;

#define TOUCH_WAKE_MAGIC 0x54574b50u   /* 'TWKP' */

/* True if the wake stub captured a raw point this wake. Clears the stash so a
 * later wake without a stub capture does not replay a stale point. */
bool touch_wakestub_take(int *rx, int *ry);
