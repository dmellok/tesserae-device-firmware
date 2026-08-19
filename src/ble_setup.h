/* Bounded BLE setup and maintenance service. */
#pragma once

#include <stdint.h>

#include "sdkconfig.h"

/* Defined on builds that actually carry BLE setup. The overlay enabling NimBLE
 * is applied only to boards with a Refresh button, so this one condition covers
 * both "the stack is linked" and "there is a control to trigger it".
 *
 * Gate UI hints and the hold gesture on THIS, not on BOARD_BTN_REFRESH_PIN: the
 * selftest envs have the button without the overlay, and there a hint would
 * advertise a gesture that reaches nothing but the ble_setup_run() stub. */
#ifdef CONFIG_BT_NIMBLE_ENABLED
#define TESSERAE_BLE_SETUP_AVAILABLE 1
#endif

typedef enum {
    BLE_SETUP_MODE_NEW_DEVICE = 1,
    BLE_SETUP_MODE_MAINTENANCE = 2,
} ble_setup_mode_t;

typedef enum {
    BLE_SETUP_RESULT_TIMEOUT = 0,
    BLE_SETUP_RESULT_CONFIGURED,
    BLE_SETUP_RESULT_REBOOT,
    BLE_SETUP_RESULT_CLEAR_WIFI,
    BLE_SETUP_RESULT_FACTORY_RESET,
    BLE_SETUP_RESULT_ERROR,
} ble_setup_result_t;

/* Starts advertising, paints the QR/passkey screen, and serves one bounded
 * session. Wi-Fi/server configuration is committed only after both tests pass. */
ble_setup_result_t ble_setup_run(ble_setup_mode_t mode, uint32_t timeout_s);
