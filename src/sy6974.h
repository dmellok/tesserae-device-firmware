/*
 * sy6974.h -- Silergy SY6974 single-cell charger, status read-out only.
 *
 * The reTerminal E10xx boards charge through this part (7-bit 0x6B). It is the
 * only place the firmware can learn whether USB is attached and whether the
 * cell is actually taking charge; the ADC divider on GPIO1 sees only voltage.
 *
 * READ-ONLY, deliberately. Seeed's SenseCraft firmware disables the charger
 * watchdog and rewrites the input / charge current limits at boot
 * (src/boards/common/pmic_sy6974.cpp). We do not: the chip's defaults charge
 * the pack correctly on its own, and a wrong current limit is a way to cook a
 * cell from software. Nothing here writes a register.
 *
 * Two registers, one transaction each:
 *   REG08 status  bits 7:5 VBUS_STAT (0 = no input), bits 4:3 CHRG_STAT
 *                 (0 idle, 1 pre-charge, 2 fast charge, 3 done), bit 2 PG,
 *                 bit 1 THERM, bit 0 VSYS regulation
 *   REG09 faults  bit 3 BAT_FAULT (over-voltage), bits 2:0 NTC_FAULT
 * Bit positions per Seeed's lib/SY6974/SY6974.cpp (getBusStatus,
 * getChargeStatus) and pmic_sy6974.cpp (isBatteryConnected). Seeed's
 * isCharging() returns "VBUS present", which is not the same thing; the
 * `charging` flag here comes from CHRG_STAT.
 *
 * Board configuration (boards/seeed_reterminal_e100x.h):
 *   BOARD_HAS_SY6974         enables this driver
 *   BOARD_SY6974_I2C_PORT    I2C port, shared via i2c_bus_get(). NOTE: on the
 *                            E1001 / E1002 the charger sits on its own bus
 *                            (port 1, SDA 39 / SCL 40), not the sensor bus.
 *   BOARD_SY6974_I2C_SDA/SCL pins
 *   BOARD_SY6974_I2C_HZ      bus speed for this device
 *   BOARD_SY6974_I2C_ADDR    0x6B (fixed in hardware)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"   /* boards/board.h: BOARD_HAS_SY6974 + pins */

typedef struct {
    bool vbus_present;    /* REG08 VBUS_STAT != 0: something on the USB port */
    bool charging;        /* REG08 CHRG_STAT is pre-charge or fast charge */
    bool charge_done;     /* REG08 CHRG_STAT == charge termination */
    bool battery_fault;   /* REG09 BAT_FAULT or any NTC_FAULT (cell missing,
                             over-temperature, over-voltage) */
    uint8_t reg08;        /* raw status, for the log */
    uint8_t reg09;        /* raw faults, for the log */
} sy6974_status_t;

#ifdef BOARD_HAS_SY6974

/* Read REG08 and REG09 and decode them. Returns false, leaving *out untouched,
 * if the chip does not answer. */
bool sy6974_read(sy6974_status_t *out);

/* Whether the chip answered its address. Probed once, then cached. */
bool sy6974_present(void);

#else  /* no SY6974 on this board: compile to nothing */

static inline bool sy6974_read(sy6974_status_t *out) { (void)out; return false; }
static inline bool sy6974_present(void)               { return false; }

#endif
