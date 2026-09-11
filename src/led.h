/*
 * led.h -- the single status LED some boards carry.
 *
 * The reTerminal E10xx boards have one green LED next to the buttons. The
 * firmware uses it as a boot indicator (on from led_init() until the first
 * frame is up) and for the occasional blink pattern; nothing else, because a
 * lit LED on a sleeping e-ink panel is a battery drain the user cannot see the
 * point of. led_prepare_sleep() therefore guarantees the pin draws nothing
 * across deep sleep.
 *
 * Board configuration (boards/seeed_reterminal_e100x.h):
 *   BOARD_LED_PIN          GPIO; defining it enables this module
 *   BOARD_LED_ACTIVE_LOW   1 when the LED is wired pin -> 3V3 and lights on a
 *                          low output (all reTerminals per Seeed's
 *                          examples/base/LED_Control/LED_Control.ino)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"   /* boards/board.h: BOARD_LED_PIN */

#ifdef BOARD_LED_PIN

/* Configure the pin as an output and light the LED (boot indicator). */
void led_init(void);

/* Steady on or off. Cancels any blink in progress. */
void led_set(bool on);

/* Blink from an esp_timer: on for `on_ms`, off for the rest of `period_ms`,
 * repeating. `on_ms` is clamped to the period. period_ms == 0 stops the blink
 * and turns the LED off. */
void led_blink(uint32_t on_ms, uint32_t period_ms);

/* Off, blink stopped, and the pin parked so it sources or sinks nothing
 * through deep sleep. Call from the sleep path, after any final blink. */
void led_prepare_sleep(void);

#else  /* no LED on this board: compile to nothing */

static inline void led_init(void)                                   {}
static inline void led_set(bool on)                                 { (void)on; }
static inline void led_blink(uint32_t on_ms, uint32_t period_ms)    { (void)on_ms; (void)period_ms; }
static inline void led_prepare_sleep(void)                          {}

#endif
