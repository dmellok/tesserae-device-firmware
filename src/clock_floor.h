/* A persisted high-water mark for wall-clock time.
 *
 * The certificate-date check trusts the system clock, and the clock is only
 * *plausible* -- the pre-TLS gate accepts anything inside a wide window, so a
 * wrong-but-sane clock passes. The dangerous direction is backwards: winding the
 * clock to before a certificate's expiry makes an expired certificate look
 * current, which is exactly what the date check exists to stop.
 *
 * This module remembers the latest time the device has seen from a source it had
 * already accepted -- the Date header of a *verified* HTTPS response -- and
 * refuses a clock that falls materially below it. Stored in NVS, so it survives
 * deep sleep, reboot and power loss.
 *
 * What it is NOT:
 *
 *   - It is not authenticated time. The mark is only as good as the last verified
 *     response, and the Date header comes from a server whose certificate was
 *     checked against whatever clock the device had at that moment.
 *   - It does not prove the clock is *right*, only that it is not *earlier* than
 *     anything already seen. A clock that is uniformly too early -- RTC never
 *     set, never corrected -- passes both checks.
 *   - It does not survive an NVS erase. A factory reset forgets it, exactly like
 *     every other setting.
 *
 * The residual limitations are documented in the stage report; this header states
 * them so the code does not read as a stronger guarantee than it is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How far below the mark a clock may sit before it is treated as rolled back.
 * Five minutes absorbs NTP jitter, the Date header's one-second resolution and
 * the gap between a wake and its request; it is four orders of magnitude smaller
 * than any certificate validity window it could help an attacker with. */
#define CLOCK_FLOOR_TOLERANCE_S 300

/* A new mark is persisted at most once a day. One 8-byte NVS write a day is
 * nothing against the flash's endurance, and it keeps a device that wakes every
 * fifteen minutes from writing on every wake. */
#define CLOCK_FLOOR_WRITE_EVERY_S 86400

/* Anything outside this window is not a mark, it is corruption. */
#define CLOCK_FLOOR_MIN_EPOCH 1735689600LL   /* 2025-01-01 */
#define CLOCK_FLOOR_MAX_EPOCH 4102444800LL   /* 2100-01-01 */

typedef struct {
    bool present;     /* false when NVS has nothing, or nothing usable */
    int64_t epoch;
} clock_floor_t;

/* No mark yet: this is a fresh device, and the bootstrap policy applies --
 * plausibility plus the existing pre-TLS gate. */
clock_floor_t clock_floor_none(void);

/* Would `candidate` be accepted against this mark? A missing mark accepts
 * anything (the gate above still has to agree). */
bool clock_floor_accepts(clock_floor_t floor, int64_t candidate);

/* Is this observation worth persisting? Only forward progress, and only when it
 * is more than a day beyond what is stored. */
bool clock_floor_should_advance(clock_floor_t floor, int64_t observed);

/* Is a stored value usable, or is it corruption? */
bool clock_floor_value_ok(int64_t epoch);

#ifdef ESP_PLATFORM
/* NVS-backed; never fatal, and never silently downgrades a present mark to a
 * missing one without saying so in the log. */
clock_floor_t clock_floor_load(void);

/* Persist `observed` if it is due. Returns true when a write happened. */
bool clock_floor_advance(int64_t observed);
#endif
