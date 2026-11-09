/* Host tests for the persisted clock high-water mark.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I src test/test_clock_floor.c src/clock_floor.c \
 *      -o /tmp/tcf && /tmp/tcf
 *
 * The NVS layer is not compiled here; what is tested is the arithmetic that
 * decides whether a clock is acceptable and whether a mark is worth writing.
 * The cases that matter on a panel are the ones a panel makes expensive to
 * produce: a clock wound backwards, a mark that has not moved for a year, and
 * a first boot that has no mark at all.
 */

#include <inttypes.h>
#include <stdio.h>

#include "clock_floor.h"

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

#define T0 1767225600LL              /* 2026-01-01T00:00:00Z */
#define DAY 86400LL

static clock_floor_t marked(int64_t epoch)
{
    clock_floor_t f = { .present = true, .epoch = epoch };
    return f;
}

/* ---- bootstrap: no mark at all ---------------------------------------- */

static void test_no_mark_accepts_anything(void)
{
    const clock_floor_t none = clock_floor_none();
    CHECK(!none.present);
    CHECK(clock_floor_accepts(none, T0));            /* the gate still decides */
    CHECK(clock_floor_accepts(none, 0));             /* even "no clock", as before */
    CHECK(clock_floor_accepts(none, T0 + 40 * 365 * DAY));
}

static void test_first_observation_creates_a_mark(void)
{
    const clock_floor_t none = clock_floor_none();
    CHECK(clock_floor_should_advance(none, T0));
    CHECK(clock_floor_should_advance(none, T0 + 10 * DAY));   /* latest wins */
}

/* ---- rollback detection ------------------------------------------------ */

static void test_clock_behind_the_mark_is_refused(void)
{
    const clock_floor_t f = marked(T0);
    CHECK(clock_floor_accepts(f, T0));               /* exactly at the mark */
    CHECK(clock_floor_accepts(f, T0 + 1));
    CHECK(!clock_floor_accepts(f, T0 - CLOCK_FLOOR_TOLERANCE_S - 1));
    CHECK(!clock_floor_accepts(f, T0 - 180 * DAY));  /* the attack */
    CHECK(!clock_floor_accepts(f, 0));               /* no clock at all */
}

static void test_tolerance_edges(void)
{
    const clock_floor_t f = marked(T0);
    CHECK(clock_floor_accepts(f, T0 - CLOCK_FLOOR_TOLERANCE_S));      /* just inside */
    CHECK(!clock_floor_accepts(f, T0 - CLOCK_FLOOR_TOLERANCE_S - 1)); /* just outside */
}

static void test_rollback_before_expiry_is_the_case_that_matters(void)
{
    /* An expired certificate (expired at T0 + 30 days) plus a clock wound back
     * to T0 - 60 days: without a mark that is "plausible" and the certificate
     * would be accepted. With a mark it is refused. */
    const clock_floor_t f = marked(T0 + 40 * DAY);
    CHECK(!clock_floor_accepts(f, T0 - 60 * DAY));
}

/* ---- the limitation this design does NOT remove ------------------------ */

static void test_both_old_is_still_vulnerable(void)
{
    /* A device that has been offline for a year, whose RTC died and reset to a
     * plausible-but-old date, with a mark from the same era: the clock is
     * uniformly too early, so it sits at or above the mark and is accepted.
     * An expired certificate could then look current. This is the freshness
     * limitation, encoded as a test so nobody later reads the mark as proof of
     * correct time: a high-water mark bounds rollback, not staleness. */
    const clock_floor_t f = marked(T0);            /* mark from a year ago */
    const int64_t rtc_year_old = T0 + DAY;         /* RTC also from that era */
    CHECK(clock_floor_accepts(f, rtc_year_old));   /* and that is the gap */

    /* Closing it would need a maximum age, which is exactly what the stage
     * rejected because a LAN without NTP would then stop working entirely. */
}

/* ---- advancing the mark ------------------------------------------------ */

static void test_mark_advances_only_forward_and_by_a_day(void)
{
    const clock_floor_t f = marked(T0);
    CHECK(!clock_floor_should_advance(f, T0));                 /* same */
    CHECK(!clock_floor_should_advance(f, T0 - 30 * DAY));      /* backwards */
    CHECK(!clock_floor_should_advance(f, T0 + 60));            /* 1 min: not due */
    CHECK(!clock_floor_should_advance(f, T0 + DAY));           /* exactly a day: not yet */
    CHECK(clock_floor_should_advance(f, T0 + DAY + 1));        /* due */
    CHECK(clock_floor_should_advance(f, T0 + 10 * DAY));
}

static void test_daily_write_bound(void)
{
    /* Simulate a month of fifteen-minute wakes: the mark must move once a day
     * at most, not 96 times. */
    clock_floor_t f = marked(T0);
    int writes = 0;
    for (int wake = 1; wake <= 30 * 96; wake++) {
        const int64_t now = T0 + (int64_t)wake * 900;
        if (clock_floor_should_advance(f, now)) {
            f = marked(now);
            writes++;
        }
    }
    CHECK(writes <= 30);
    CHECK(writes >= 28);        /* and it still tracks the clock */
}

static void test_junk_is_never_stored(void)
{
    const clock_floor_t f = marked(T0);
    CHECK(!clock_floor_should_advance(f, 0));
    CHECK(!clock_floor_should_advance(f, -1));
    CHECK(!clock_floor_should_advance(f, 1000));                       /* 1970 */
    CHECK(!clock_floor_should_advance(f, CLOCK_FLOOR_MAX_EPOCH + 1));  /* year 2100+ */
    CHECK(!clock_floor_should_advance(clock_floor_none(), 12345));
}

static void test_stored_value_validation(void)
{
    CHECK(!clock_floor_value_ok(0));
    CHECK(!clock_floor_value_ok(946684800LL));      /* 2000: chip power-up value */
    CHECK(clock_floor_value_ok(CLOCK_FLOOR_MIN_EPOCH));
    CHECK(clock_floor_value_ok(T0));
    CHECK(clock_floor_value_ok(CLOCK_FLOOR_MAX_EPOCH));
    CHECK(!clock_floor_value_ok(CLOCK_FLOOR_MAX_EPOCH + 1));
    CHECK(!clock_floor_value_ok(-5));
}

int main(void)
{
    test_no_mark_accepts_anything();
    test_first_observation_creates_a_mark();
    test_clock_behind_the_mark_is_refused();
    test_tolerance_edges();
    test_rollback_before_expiry_is_the_case_that_matters();
    test_both_old_is_still_vulnerable();
    test_mark_advances_only_forward_and_by_a_day();
    test_daily_write_bound();
    test_junk_is_never_stored();
    test_stored_value_validation();

    printf("clock_floor: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
