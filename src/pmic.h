/*
 * AXP2101 PMIC wrapper (boards that define BOARD_HAS_PMIC).
 *
 * On the Waveshare PhotoPainter 7.3" the panel/SD/codec rails and the
 * battery gauge are all behind an AXP2101 over I2C -- there is no GPIO
 * panel-power gate and no ADC battery divider like the reTerminal boards.
 * This wrapper is intentionally minimal: bring the analog rails up before a
 * render, read the battery for the status heartbeat, and (optionally) drop
 * the rails for deep sleep.
 *
 * Ported from tesserae-photopainter-7.3-bin-client/src/pmic.c. Boards that do
 * NOT define BOARD_HAS_PMIC link no-op stubs, so callers can reference these
 * unconditionally without pulling in I2C on ADC-battery boards.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Bring the AXP2101 up over I2C and put it in a known state (VBUS 2 A cap,
 * ALDO1..4 = 3.3 V enabled, charge 500 mA / 25 mA termination). Idempotent:
 * safe to call once per cold boot and after every deep-sleep wake, and cheap
 * on repeat calls. Returns ESP_OK, or ESP_FAIL if the chip never ACKs (treat
 * as "no PMIC" -- log and keep going). No-op ESP_OK on non-PMIC boards. */
esp_err_t pmic_init(void);

/* Toggle the analog-rail LDOs (ALDO1..4). pmic_rails_set(false) before
 * esp_deep_sleep_start() sheds the panel/SD/codec draw. No-op on non-PMIC. */
esp_err_t pmic_rails_set(bool enabled);

/* Battery millivolts from the AXP2101 VBAT ADC (regs 0x34/0x35), or 0 if no
 * battery / read fails / non-PMIC board. */
uint16_t pmic_battery_mv(void);

/* 0..100 state-of-charge from a 1S LiPo VBAT curve, or -1 if unavailable.
 * (Not the AXP coulomb counter, which needs a learning cycle we don't run.) */
int pmic_battery_pct(void);

/* True iff a battery is detected on the AXP2101 BATSENSE pin. */
bool pmic_battery_present(void);
