/*
 * power_latch.h -- optional battery self-latch, for boards where the cell
 * reaches the regulator through a MOSFET the MCU has to hold on itself.
 *
 *     #define BOARD_POWER_LATCH_PIN  <gpio>   // drive HIGH to stay powered
 *
 * or, for a clocked latch (data captured on a clock edge):
 *
 *     #define BOARD_POWER_LATCH_DATA_PIN <gpio>
 *     #define BOARD_POWER_LATCH_CLK_PIN  <gpio>
 *
 * There the power button only bridges the rail while held: asserting this pin
 * is what keeps the device alive after the user lets go, and dropping it is a
 * real power-off -- the MCU and its RTC domain lose power, not just sleep.
 *
 * Header-only, compiles to nothing without the macro. See boards/xteink_x4.h.
 */
#pragma once

#include "app_config.h"   /* -> board.h : BOARD_POWER_LATCH_PIN */

#ifdef BOARD_POWER_LATCH_PIN

#include "driver/gpio.h"

/* Assert and keep asserted. Call FIRST in app_main(): on a unit that does not
 * self-latch, everything before this runs only while the button is held.
 * gpio_hold_dis() first, or a hold left from the last sleep makes the write a
 * no-op. Harmless on units that do self-latch. */
static inline void power_latch_hold(void)
{
    gpio_hold_dis((gpio_num_t)BOARD_POWER_LATCH_PIN);
    gpio_set_direction((gpio_num_t)BOARD_POWER_LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_PIN, 1);
}

/* Latch HIGH across deep sleep, so the rail survives and the wake source has an
 * MCU left to wake. Without it the pad floats at sleep entry and a timed nap
 * becomes a power-off. Call immediately before esp_deep_sleep_start(). */
static inline void power_latch_hold_through_sleep(void)
{
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_PIN, 1);
    gpio_hold_en((gpio_num_t)BOARD_POWER_LATCH_PIN);
    gpio_deep_sleep_hold_en();
}

/* Drop the rail: genuinely off, drawing nothing. For the paths that mean "stop
 * until a human intervenes", where off beats a sleep no timer will end. */
static inline void power_latch_release(void)
{
    gpio_hold_dis((gpio_num_t)BOARD_POWER_LATCH_PIN);
    gpio_set_direction((gpio_num_t)BOARD_POWER_LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_PIN, 0);
    gpio_hold_en((gpio_num_t)BOARD_POWER_LATCH_PIN);
    gpio_deep_sleep_hold_en();
}

#elif defined(BOARD_POWER_LATCH_DATA_PIN) && defined(BOARD_POWER_LATCH_CLK_PIN)

/* Clocked latch (reTerminal Sticky, per the TRMNL Sticky build, bl.cpp:
 * "pinMode(45) power hold (DATA); pinMode(46) power lock (CLK); 45=1; 46=0;
 * 46=1 -- hold the battery power enabled for after the user releases the
 * power button"). The DATA level is captured on the CLK rising edge, so the
 * hold is a state inside the latch, not a level the MCU has to keep driving:
 * nothing needs to survive deep sleep. Release clocks a 0 through the same
 * way. UNVERIFIED on our own bench (2026-09-11); our Sticky units have only
 * ever been run on USB or with the button held, which is exactly what this
 * would hide. */

#include "driver/gpio.h"
#include "rom/ets_sys.h"

static inline void power_latch_clock(int data)
{
    gpio_hold_dis((gpio_num_t)BOARD_POWER_LATCH_DATA_PIN);
    gpio_hold_dis((gpio_num_t)BOARD_POWER_LATCH_CLK_PIN);
    gpio_set_direction((gpio_num_t)BOARD_POWER_LATCH_DATA_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)BOARD_POWER_LATCH_CLK_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_DATA_PIN, data);
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_CLK_PIN, 0);
    ets_delay_us(10);
    gpio_set_level((gpio_num_t)BOARD_POWER_LATCH_CLK_PIN, 1);
    ets_delay_us(10);
}

static inline void power_latch_hold(void) { power_latch_clock(1); }
/* Held inside the latch; keep the pins quiet through sleep all the same. */
static inline void power_latch_hold_through_sleep(void)
{
    gpio_hold_en((gpio_num_t)BOARD_POWER_LATCH_DATA_PIN);
    gpio_hold_en((gpio_num_t)BOARD_POWER_LATCH_CLK_PIN);
    gpio_deep_sleep_hold_en();
}
static inline void power_latch_release(void) { power_latch_clock(0); }

#else  /* no latch */

static inline void power_latch_hold(void) { }
static inline void power_latch_hold_through_sleep(void) { }
static inline void power_latch_release(void) { }

#endif /* BOARD_POWER_LATCH_PIN */
