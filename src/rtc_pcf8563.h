/*
 * rtc_pcf8563.h -- NXP PCF8563 real-time clock with a coin-cell backup.
 *
 * The reTerminal E10xx boards carry one on I2C0 (SDA 19 / SCL 20, 7-bit 0x51)
 * with a CR1220 behind it, so wall-clock time survives a flat main battery and
 * a cold boot. Two directions:
 *
 *   seed_clock  RTC -> system clock, before Wi-Fi is up. Lets the first frame
 *               carry a real timestamp and lets wake alignment run on a boot
 *               that never reached NTP.
 *   store_clock system clock -> RTC, after a successful NTP sync. The same
 *               write-back-after-sync pattern Seeed's SenseCraft firmware uses
 *               (src/APP/app_sensecraft.cpp, after settimeofday()).
 *
 * The RTC holds UTC, not local time. Seeed's example stores whatever the
 * system clock says; ours says UTC, and keeping it that way means a timezone
 * change on the server never invalidates the stored value.
 *
 * Register map and init sequence follow Seeed's
 * examples/base/RTC_PCF8563/RTC_PCF8563.ino: clear STOP (CTRL1 = 0), clear the
 * alarm/timer flags (CTRL2 = 0), switch CLKOUT off (0x0D = 0), read the seven
 * BCD time registers in one burst from 0x02, and trust them only while the
 * VL (voltage-low) bit in the seconds register is clear.
 *
 * Board configuration (boards/seeed_reterminal_e100x.h):
 *   BOARD_HAS_PCF8563         enables this driver
 *   BOARD_PCF8563_I2C_PORT    I2C port, shared via i2c_bus_get()
 *   BOARD_PCF8563_I2C_SDA/SCL pins
 *   BOARD_PCF8563_I2C_HZ      bus speed for this device
 *   BOARD_PCF8563_I2C_ADDR    0x51 (fixed in hardware)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"   /* boards/board.h: BOARD_HAS_PCF8563 + pins */

#ifdef BOARD_HAS_PCF8563

/* Read the RTC and, if it answers, its backup never ran flat (VL clear) and the
 * stored year is at least 2025, settimeofday() from it. Returns true only when
 * the system clock was set. Each call also re-runs the one-off init (STOP
 * cleared, CTRL2 cleared, CLKOUT off) when the chip answers; the writes are
 * idempotent and cheap. */
bool rtc_pcf8563_seed_clock(void);

/* Write the current system time (gettimeofday(), UTC) into the RTC. Returns
 * true on a successful burst write. Call after an NTP sync, not before. */
bool rtc_pcf8563_store_clock(void);

/* Whether the chip answered its address. Probed once, then cached. */
bool rtc_pcf8563_present(void);

/* Countdown timer -> INT, for boards whose INT line feeds the power-on
 * circuit (the M5Paper): the wake-up call for an RTC-timed power-off.
 * Programs the timer at 1 Hz for up to 255 s, else at 1/60 Hz in whole
 * minutes rounded up (to 255 min; longer is capped, the board wakes early and
 * simply sleeps again), enables the timer interrupt and clears any stale
 * timer / alarm flag. The 1/60 Hz source has no known phase, so a minute-
 * resolution wake can land up to a minute late. Returns the seconds actually
 * programmed, 0 on failure. */
uint32_t rtc_pcf8563_arm_wake_timer(uint32_t seconds);

/* Countdown timer off and its flag cleared, for a power-off that should last
 * until a person presses the button. Returns false if the RTC did not answer. */
bool rtc_pcf8563_disarm_wake_timer(void);

/* Whether the countdown timer had fired when this boot first looked (the TF
 * flag in CTRL2). On a board that powers on from INT, true means this
 * power-on was the timer's, not a person's. Read once and cached; every init
 * afterwards clears the flag and disables the timer, so call this before
 * rtc_pcf8563_seed_clock() or rely on the cache. */
bool rtc_pcf8563_woke_by_timer(void);

#else  /* no RTC on this board: compile to nothing */

static inline bool rtc_pcf8563_seed_clock(void)  { return false; }
static inline bool rtc_pcf8563_store_clock(void) { return false; }
static inline bool rtc_pcf8563_present(void)     { return false; }
static inline uint32_t rtc_pcf8563_arm_wake_timer(uint32_t seconds) { (void)seconds; return 0; }
static inline bool rtc_pcf8563_disarm_wake_timer(void) { return false; }
static inline bool rtc_pcf8563_woke_by_timer(void) { return false; }

#endif
