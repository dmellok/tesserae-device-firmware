/*
 * diag_run.h: the firmware side of the detected-failure latch (diag.h). The
 * state lives in RTC_NOINIT memory, so a latch survives deep sleep, a panic
 * and a software reset (the brownout detector's ISR ends in one) and is lost
 * on power-off.
 *
 * Panel drivers call diag_note_paint() from their timeout branches; it is
 * cheap and quiet after the first call, so a wait loop that times out on every
 * command is fine. net_rest adds the report to the /status body and clears it
 * once a status carrying it comes back 2xx.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"

/* Validate the retained latch and record this boot's reset reason when it
 * was a brownout, panic or watchdog. Called once from app_main; the other
 * entry points call it themselves if a driver gets there first. */
void diag_run_boot(void);

/* A panel driver detected a failure. */
void diag_note_paint(diag_paint_t paint);

/* diag_note_paint() calls since boot, whether or not they changed the latch.
 * Compare before and after a paint to learn whether that paint failed. */
uint32_t diag_run_paint_failures(void);

/* The report waiting for delivery, if any. */
bool diag_run_report(diag_report_t *out);

/* A status body carrying report `id` was accepted (2xx). */
void diag_run_clear(uint32_t id);
