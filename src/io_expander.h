/*
 * io_expander.h -- register access for the PCA9535 / PCAL6416A 16-bit I/O
 * expanders. The two share the base register map (input 0/1, output 2/3,
 * config 6/7; config bit 1 = input), so one helper serves the epdiy v7 PCA9535
 * and the Inkplate 13SPECTRA PCAL6416A. Port 1 is expander pins 8..15.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define IO_EXP_REG_IN1   0x01
#define IO_EXP_REG_OUT1  0x03
#define IO_EXP_REG_CFG1  0x07

typedef struct { i2c_master_dev_handle_t dev; } io_exp_t;

/* Attach the expander at `addr` on the shared bus (i2c_bus.h). */
esp_err_t io_exp_open(io_exp_t *x, int port, int sda, int scl, uint8_t addr, uint32_t hz);
bool io_exp_write(const io_exp_t *x, uint8_t reg, uint8_t val);
bool io_exp_read(const io_exp_t *x, uint8_t reg, uint8_t *val);
