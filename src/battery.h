/*
 * battery.h: optional Li-Po battery telemetry for the status heartbeat.
 *
 * The ADC channel is board-specific, so it is supplied by the board header via
 * BOARD_BATTERY_ADC_CHANNEL (+ optional BOARD_BATTERY_DIVIDER and
 * BOARD_VBAT_SWITCH_PIN). Boards that don't define a channel report 0 mV, which
 * the server treats as "unknown" rather than "empty".
 *
 * Lifted from the old heartbeat.c (Waveshare 13.3E6 ADC reference).
 */
#pragma once

#include <stdbool.h>

/* True when this board can actually measure its cell.
 *
 * Check this before acting on battery_read_mv(). A board with no sense reports
 * 0 mV, which is indistinguishable from a flat cell, and treating it as a
 * reading means "0%" everywhere: the XIAO C3 panel has a battery but no divider
 * to any ADC pin, so it can never report one. */
bool battery_present(void);

/* Battery rail in millivolts, or 0 if this board has no configured sense. */
int battery_read_mv(void);

/* Map a Li-Po cell voltage (mV) to a 0-100% state-of-charge estimate. */
int battery_pct(int mv);

#ifdef BATTERY_DEBUG_SWEEP
/* Diagnostic: loop forever logging raw + calibrated mV across every ADC1
 * channel (with the board's load switch enabled), to identify the real
 * battery sense pin and divider. Enable with -DBATTERY_DEBUG_SWEEP. */
void battery_debug_sweep(void);
#endif
