/* Minimal SHTC3 temperature/humidity reader for sensor-equipped boards. */
#pragma once

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_pct;
} shtc3_sample_t;

/* Read one sample (normal mode, temperature first). Returns
 * ESP_ERR_NOT_SUPPORTED on boards without BOARD_HAS_SHTC3 and leaves the
 * output untouched on failure. */
esp_err_t shtc3_read(shtc3_sample_t *out);
