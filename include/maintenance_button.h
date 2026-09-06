// Debounced short-press exit, independent of GPIO and the RTOS.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAINTENANCE_BUTTON_DEBOUNCE_MS 50u
#define MAINTENANCE_BUTTON_HOLD_MS 3000u

typedef struct {
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool press_active;
    uint32_t changed_at;
    uint32_t pressed_at;
} maintenance_button_t;

static inline maintenance_button_t maintenance_button_init(bool pressed, uint32_t now)
{
    // Require a stable release after the maintenance screen is ready. The
    // hold that opened this session must never double as an exit press.
    return (maintenance_button_t){
        .raw_pressed = pressed, .stable_pressed = true, .changed_at = now,
    };
}

static inline bool maintenance_button_poll(maintenance_button_t *button,
                                           bool pressed, uint32_t now)
{
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->changed_at = now;
    }
    if ((uint32_t)(now - button->changed_at) < MAINTENANCE_BUTTON_DEBOUNCE_MS ||
        pressed == button->stable_pressed)
        return false;

    button->stable_pressed = pressed;
    if (!button->armed) {
        if (!pressed) button->armed = true;
        return false;
    }
    if (pressed) {
        button->press_active = true;
        button->pressed_at = button->changed_at;
        return false;
    }
    if (!button->press_active) return false;
    button->press_active = false;
    uint32_t held_ms = button->changed_at - button->pressed_at;
    // Long holds do nothing inside a session, including the reset gesture.
    return held_ms >= MAINTENANCE_BUTTON_DEBOUNCE_MS &&
           held_ms < MAINTENANCE_BUTTON_HOLD_MS;
}
