/*
 * battery.h: optional Li-Po battery telemetry for the status heartbeat.
 *
 * Three backends, one per way a board can see its cell, selected by what the
 * board header defines:
 *
 *   BOARD_BATTERY_ADC_CHANNEL   divided cell voltage on an ADC pin (+ optional
 *                               BOARD_BATTERY_DIVIDER, BOARD_VBAT_SWITCH_PIN)
 *   BOARD_BATTERY_PMIC          AXP2101 PMIC over I2C  (pmic.c)
 *   BOARD_BATTERY_GAUGE_I2C     BQ27220 fuel gauge over I2C  (bq27220.c)
 *
 * A board that defines none of them reports no battery at all, which is a real
 * configuration rather than a gap: see battery_present().
 *
 * Lifted from the old heartbeat.c (Waveshare 13.3E6 ADC reference).
 */
#pragma once

#include <stdbool.h>

/* True when this board can actually measure its cell RIGHT NOW.
 *
 * Check this before acting on battery_read_mv() or battery_read_pct(). A board
 * with no sense reports 0 mV, which is indistinguishable from a flat cell, and
 * treating it as a reading means "0%" everywhere: the XIAO C3 panel has a
 * battery but no divider to any ADC pin, so it can never report one.
 *
 * On the ADC and PMIC boards this is a compile-time fact. On a gauge board it
 * is answered at runtime, because a gauge that is busy or unconfigured declines
 * to reply, and we would rather publish nothing than a fictional flat cell. */
bool battery_present(void);

/* Battery rail in millivolts, or 0 if this board has no configured sense. */
int battery_read_mv(void);

/* Map a Li-Po cell voltage (mV) to a 0-100% state-of-charge estimate. */
int battery_pct(int mv);

/* This board's state of charge, 0-100.
 *
 * Prefer this over battery_pct(battery_read_mv()). On a fuel-gauge board it
 * returns the gauge's coulomb-counted figure, which knows the pack's real
 * capacity and the current draw; the voltage curve is only an approximation
 * standing in where there is nothing better. Falls back to that curve on every
 * other board, so callers need not care which they are on. */
int battery_read_pct(void);

/* ---------- always-on eligibility ---------- */

/* True when this device may advertise `can_stay_awake` to the server, which
 * every board now does unless a visible cell is running down.
 *
 * The capability used to be a board-header assertion (BOARD_MAINS_POWERED,
 * E1003 only), on the theory that only a board KNOWN to be mains-wired should
 * offer a mode that flattens a battery in hours. But there is no VBUS or
 * mains sense anywhere in this firmware -- the one signal that looks like it
 * would do, usb_serial_jtag_is_connected(), detects a USB DATA HOST, not
 * power, and a panel on a wall charger reports disconnected. Since the
 * firmware cannot tell a wall-wart from a cell, the choice belongs to the
 * operator, who can: any panel may be set to stay awake, and the setting
 * stays off by default.
 *
 * Runtime still RETRACTS the capability: if a cell is visible and drops below
 * AWAKE_BATTERY_MIN_PCT we stop advertising it and fall back to deep sleep
 * whatever the server thinks, because protecting the pack outranks honouring
 * the setting. So the worst an inadvisable always-on costs a battery panel is
 * charge down to that floor, not the whole pack. */
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
