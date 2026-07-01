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

/* Battery rail in millivolts, or 0 if this board has no configured sense. */
int battery_read_mv(void);

/* Map a Li-Po cell voltage (mV) to a 0-100% state-of-charge estimate. */
int battery_pct(int mv);
