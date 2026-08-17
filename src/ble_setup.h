/* Bounded BLE setup and maintenance service. */
#pragma once

#include <stdint.h>

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
