/* buzzer.h -- immediate audible feedback for touches and button presses.
 *
 * E-ink takes seconds to repaint, so a tap gives the user nothing to go on
 * until the whole pipeline (dispatch, render, download, flash) completes.
 * The reTerminal E series carries a passive piezo on GPIO45; sounding it the
 * instant the input is registered closes that gap locally, with no server
 * round trip involved (server #258).
 *
 * Passive, so pitch is the PWM frequency and loudness is the duty cycle. The
 * tone names come from the server config as strings rather than frequencies:
 * the board owns the pitch and envelope of each, so hardware with a different
 * resonant peak can voice "click" its own way without a stored config on the
 * server becoming wrong.
 *
 * Boards without BOARD_BUZZER_PIN compile every call away to nothing.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_config.h"

#ifdef BOARD_BUZZER_PIN

/* Sound the configured tone at the configured volume. No-op when the beep is
 * disabled or the volume is 0. Blocks for the length of the tone (tens of ms:
 * short enough to sit in the touch path, which is the only place it can be
 * and still mean anything). Safe to call before any other subsystem is up. */
void buzzer_feedback(void);

/* Sound one tone by name ("click" / "beep" / "chirp" / "low") at `volume`
 * percent, ignoring the stored enable flag. Used by the selftest build and by
 * the config path to preview a tone the operator just picked. An unknown name
 * falls back to "beep"; volume is clamped to 0..100. */
void buzzer_play(const char *tone, int volume);

/* Release the LEDC channel and park the pin low. Called before deep sleep so
 * the piezo cannot be left driven. */
void buzzer_idle(void);

#else /* no buzzer on this board */

static inline void buzzer_feedback(void) { }
static inline void buzzer_play(const char *tone, int volume) { (void)tone; (void)volume; }
static inline void buzzer_idle(void) { }

#endif /* BOARD_BUZZER_PIN */
