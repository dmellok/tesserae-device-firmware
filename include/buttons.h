/*
 * buttons.h -- board-agnostic front-button support (header-only).
 *
 * A board header defines any subset of these active-low, RTC-capable GPIOs:
 *
 *     #define BOARD_BTN_REFRESH_PIN  <gpio>   // middle / "green" -> refresh
 *     #define BOARD_BTN_LEFT_PIN     <gpio>   // -> rotate to previous
 *     #define BOARD_BTN_RIGHT_PIN    <gpio>   // -> rotate to next
 *
 * All defined pins are ORed into ONE ext1 deep-sleep wake mask
 * (ESP_EXT1_WAKEUP_ANY_LOW). On the next boot buttons_which_woke() reports which
 * one triggered, so the cycle can tell the server ("refresh"/"left"/"right"); the
 * server maps those to refresh / rotate_prev / rotate_next (mapping is
 * configurable server-side, so the labels are just conventions).
 *
 * Header-only and all-static-inline so it drops into main.c with no build-system
 * change, and compiles out to nothing on boards that define no buttons.
 */
#pragma once

#include "app_config.h"   /* -> board.h : BOARD_BTN_*_PIN */

#if defined(BOARD_BTN_REFRESH_PIN) || defined(BOARD_BTN_LEFT_PIN) || defined(BOARD_BTN_RIGHT_PIN)
#define BOARD_HAS_BUTTONS 1
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_REFRESH,
    BTN_LEFT,
    BTN_RIGHT,
} button_id_t;

/* Wire name the server expects, or NULL for BTN_NONE. */
static inline const char *button_name(button_id_t b)
{
    switch (b) {
        case BTN_REFRESH: return "refresh";
        case BTN_LEFT:    return "left";
        case BTN_RIGHT:   return "right";
        default:          return NULL;
    }
}

#ifdef BOARD_HAS_BUTTONS

#include <stdint.h>
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#ifdef BOARD_BTN_REFRESH_PIN
#  define BUTTONS__REFRESH_BIT (1ULL << BOARD_BTN_REFRESH_PIN)
#else
#  define BUTTONS__REFRESH_BIT 0ULL
#endif
#ifdef BOARD_BTN_LEFT_PIN
#  define BUTTONS__LEFT_BIT (1ULL << BOARD_BTN_LEFT_PIN)
#else
#  define BUTTONS__LEFT_BIT 0ULL
#endif
#ifdef BOARD_BTN_RIGHT_PIN
#  define BUTTONS__RIGHT_BIT (1ULL << BOARD_BTN_RIGHT_PIN)
#else
#  define BUTTONS__RIGHT_BIT 0ULL
#endif

#define BUTTON_WAKE_MASK (BUTTONS__REFRESH_BIT | BUTTONS__LEFT_BIT | BUTTONS__RIGHT_BIT)

/* Arm every defined button as an ext1 wake source (active-low; RTC pull-up so
 * the idle level is high and won't spuriously wake us). Call on the sleep path. */
static inline void buttons_arm_ext1(void)
{
#ifdef BOARD_BTN_REFRESH_PIN
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BTN_REFRESH_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BTN_REFRESH_PIN);
#endif
#ifdef BOARD_BTN_LEFT_PIN
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BTN_LEFT_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BTN_LEFT_PIN);
#endif
#ifdef BOARD_BTN_RIGHT_PIN
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BTN_RIGHT_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BTN_RIGHT_PIN);
#endif
    esp_sleep_enable_ext1_wakeup(BUTTON_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
}

/* Which button (if any) woke us from deep sleep. Reads the ext1 status latch;
 * if we woke on our mask but the latch is ambiguous, defaults to refresh. */
static inline button_id_t buttons_which_woke(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1)
        return BTN_NONE;
    uint64_t st = esp_sleep_get_ext1_wakeup_status();
    if (st & BUTTONS__REFRESH_BIT) return BTN_REFRESH;
    if (st & BUTTONS__LEFT_BIT)    return BTN_LEFT;
    if (st & BUTTONS__RIGHT_BIT)   return BTN_RIGHT;
    return BTN_REFRESH;   /* woke on our mask but latch unclear -> treat as refresh */
}

#else  /* !BOARD_HAS_BUTTONS */

static inline void        buttons_arm_ext1(void) { }
static inline button_id_t buttons_which_woke(void) { return BTN_NONE; }

#endif /* BOARD_HAS_BUTTONS */
