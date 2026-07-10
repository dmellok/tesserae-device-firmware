/* Minimal SHT4x temperature/humidity reader for sensor-equipped boards. */
#pragma once

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_pct;
} sht4x_sample_t;

/* Read one high-precision sample. Returns ESP_ERR_NOT_SUPPORTED on boards
 * without BOARD_HAS_SHT4X and leaves the output untouched on failure. */
esp_err_t sht4x_read(sht4x_sample_t *out);
