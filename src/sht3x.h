/* Minimal Sensirion SHT3x (SHT30/31/35) temperature/humidity reader. */
#pragma once

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_pct;
} sht3x_sample_t;

/* Read one high-repeatability single-shot sample. Returns
 * ESP_ERR_NOT_SUPPORTED on boards without BOARD_HAS_SHT3X and leaves the
 * output untouched on failure. */
esp_err_t sht3x_read(sht3x_sample_t *out);
