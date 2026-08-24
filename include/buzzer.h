/* buzzer.h -- immediate audible feedback for touches and button presses.
 *
 * E-ink takes seconds to repaint, so a tap gives the user nothing to go on
 * until the whole pipeline (dispatch, render, download, flash) completes.
 * The reTerminal E series carries a passive piezo on GPIO45; sounding it the
 * instant the input is registered closes that gap locally, with no server
 * round trip involved (server #258).
 *
 * Passive, so pitch is the PWM frequency and loudness is the duty cycle. The
 * server sends the notes themselves (``freq:ms`` steps, comma separated), not
 * a tone name: the firmware owns no tone table, so retuning a sound or adding
 * one is a settings change on the server rather than a firmware release. The
 * device only enforces the envelope it has to (note count, length, frequency
 * band) so a malformed config cannot leave the panel screaming.
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

/* Play one pattern at `volume` percent, ignoring the stored enable flag.
 * `pattern` is "freq:ms[,freq:ms...]" with frequency 0 meaning a rest; an
 * unparseable or empty pattern falls back to a plain beep, so a bad config is
 * still audible feedback rather than silence. Volume is clamped to 0..100,
 * notes to BUZZER_MAX_NOTES, and the total to BUZZER_MAX_TOTAL_MS. */
void buzzer_play(const char *pattern, int volume);

/* Release the LEDC channel and park the pin low. Called before deep sleep so
 * the piezo cannot be left driven. */
void buzzer_idle(void);

#else /* no buzzer on this board */

static inline void buzzer_feedback(void) { }
static inline void buzzer_play(const char *pattern, int volume) { (void)pattern; (void)volume; }
static inline void buzzer_idle(void) { }

#endif /* BOARD_BUZZER_PIN */
