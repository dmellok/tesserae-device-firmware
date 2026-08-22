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

/* ---------- always-on eligibility ---------- */

/* True when this board may advertise `can_stay_awake` to the server, i.e. when
 * it is running from a supply that can sustain a continuous Wi-Fi association.
 *
 * This is a statement about POWER, not about the SoC. Every board here is
 * capable of staying awake; almost none of them should, because they are
 * battery panels and a held association flattens the pack in hours.
 *
 * There is no VBUS or mains sense anywhere in this firmware, and the one signal
 * that looks like it would do -- usb_serial_jtag_is_connected() -- detects a USB
 * DATA HOST, not power. A panel on a wall charger reports disconnected, which is
 * exactly the deployment always-on exists for, so it is the wrong signal and is
 * deliberately not used here.
 *
 * So the base answer is a board-header assertion, BOARD_MAINS_POWERED, meaning
 * "this board is wired such that continuous Wi-Fi is sustainable". Runtime can
 * only ever RETRACT it: if a cell is visible and is running down, we stop
 * advertising the capability and fall back to deep sleep whatever the server
 * thinks, because protecting the pack outranks honouring the setting.
 *
 * A board that does not define BOARD_MAINS_POWERED always answers false, so
 * adding always-on to a panel is a deliberate one-line change to its header. */
bool power_can_stay_awake(void);

/* True once a visible cell has fallen far enough that always-on must stop.
 * Separated from power_can_stay_awake() so the run loop can log WHY it is
 * dropping out, and so the threshold has exactly one definition. */
bool power_battery_critical(void);

#ifdef BATTERY_DEBUG_SWEEP
/* Diagnostic: loop forever logging raw + calibrated mV across every ADC1
 * channel (with the board's load switch enabled), to identify the real
 * battery sense pin and divider. Enable with -DBATTERY_DEBUG_SWEEP. */
void battery_debug_sweep(void);
#endif
