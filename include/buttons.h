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

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
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

/* Client-side hard cap on one post-button stay-awake window (issue #123), on
 * top of the server's 0-60 s bound on button_wake_s itself: repeated presses
 * keep resetting the countdown, so this bounds the total. */
#define BUTTON_WINDOW_CAP_S 300

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

/* As buttons_arm_ext1(), but also folds extra active-low RTC-GPIO pins into the
 * same ext1 ANY_LOW mask (e.g. an active-low touch INT). RTC pull-ups keep every
 * line idling high so only a real low edge wakes us. This is the reliable wake
 * path on the reTerminal hardware (ext0 did not fire on the touch INT). */
static inline void buttons_arm_ext1_with(uint64_t extra_low_mask)
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
    for (int g = 0; g < 22; g++) {   /* RTC GPIOs 0..21 on the ESP32-S3 */
        if (extra_low_mask & (1ULL << g)) {
            rtc_gpio_pullup_en((gpio_num_t)g);
            rtc_gpio_pulldown_dis((gpio_num_t)g);
        }
    }
    esp_sleep_enable_ext1_wakeup(BUTTON_WAKE_MASK | extra_low_mask,
                                 ESP_EXT1_WAKEUP_ANY_LOW);
}

/* Configure every defined button as a plain digital input with a pull-up, for
 * live polling while awake (the post-button stay-awake window, issue #123). The
 * RTC config armed at the last sleep does not carry over to the digital GPIO
 * matrix, so this must run before buttons_poll_pressed(). */
static inline void buttons_poll_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = BUTTON_WAKE_MASK,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

/* Edge-detecting poll: reports a button once, on its high->low transition (a
 * NEW press), and not again until it is released -- so a held or stuck button
 * fires a single event. Call every ~20 ms; that cadence is also the debounce
 * (mechanical bounce settles well inside one period, and the seconds-long
 * fetch+paint after a hit swallows any release bounce). */
static inline button_id_t buttons_poll_pressed(void)
{
#ifdef BOARD_BTN_REFRESH_PIN
    {
        static bool down;
        bool p = gpio_get_level((gpio_num_t)BOARD_BTN_REFRESH_PIN) == 0;
        if (p && !down) { down = true; return BTN_REFRESH; }
        if (!p) down = false;
    }
#endif
#ifdef BOARD_BTN_LEFT_PIN
    {
        static bool down;
        bool p = gpio_get_level((gpio_num_t)BOARD_BTN_LEFT_PIN) == 0;
        if (p && !down) { down = true; return BTN_LEFT; }
        if (!p) down = false;
    }
#endif
#ifdef BOARD_BTN_RIGHT_PIN
    {
        static bool down;
        bool p = gpio_get_level((gpio_num_t)BOARD_BTN_RIGHT_PIN) == 0;
        if (p && !down) { down = true; return BTN_RIGHT; }
        if (!p) down = false;
    }
#endif
    return BTN_NONE;
}

/* Which button (if any) woke us from deep sleep. Reads the ext1 status latch.
 * Returns BTN_NONE when ext1 fired but no button bit is set -- e.g. a touch INT
 * sharing the mask -- so the caller distinguishes touch from button itself. */
static inline button_id_t buttons_which_woke(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1)
        return BTN_NONE;
    uint64_t st = esp_sleep_get_ext1_wakeup_status();
    if (st & BUTTONS__REFRESH_BIT) return BTN_REFRESH;
    if (st & BUTTONS__LEFT_BIT)    return BTN_LEFT;
    if (st & BUTTONS__RIGHT_BIT)   return BTN_RIGHT;
    return BTN_NONE;   /* ext1 fired but not a button (e.g. touch INT) */
}

#else  /* !BOARD_HAS_BUTTONS */

static inline void        buttons_arm_ext1(void) { }
static inline void        buttons_arm_ext1_with(uint64_t m) { (void)m; }
static inline button_id_t buttons_which_woke(void) { return BTN_NONE; }
static inline void        buttons_poll_init(void) { }
static inline button_id_t buttons_poll_pressed(void) { return BTN_NONE; }

#endif /* BOARD_HAS_BUTTONS */
