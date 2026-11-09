/* Host tests for the pre-TLS clock gate.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I src test/test_time_ready.c src/time_ready.c \
 *      -o /tmp/ttr && /tmp/ttr
 *
 * The cases that matter are the ones a panel on a wall cannot cheaply show: an
 * RTC that seeds a nonsense date, a network sync that answers with 1970, and a
 * clock that is plausible by accident. The firmware glue (SNTP, I2C) is not
 * compiled here; what is tested is the decision that gates every TLS request.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "time_ready.h"
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

/* ---- fake clock and sources ------------------------------------------- */

typedef struct {
    int64_t now;
    bool rtc_result;
    int64_t rtc_sets;          /* what a successful seed moves the clock to */
    bool sntp_result;
    int64_t sntp_sets;
    int rtc_calls;
    int sntp_calls;
    int last_timeout;
} fake_t;

static fake_t *g_fake;

static int64_t now_cb(void) { return g_fake->now; }

static bool rtc_cb(void)
{
    g_fake->rtc_calls++;
    if (g_fake->rtc_result) g_fake->now = g_fake->rtc_sets;
    return g_fake->rtc_result;
}

static bool sntp_cb(int timeout_ms)
{
    g_fake->sntp_calls++;
    g_fake->last_timeout = timeout_ms;
    if (g_fake->sntp_result) g_fake->now = g_fake->sntp_sets;
    return g_fake->sntp_result;
}

static void fresh(fake_t *f)
{
    memset(f, 0, sizeof *f);
    f->now = 0;
    f->rtc_sets = 1767225600LL;   /* 2026-01-01 */
    f->sntp_sets = 1767225600LL;
}

static bool evaluate(fake_t *f, int timeout_ms, time_ready_result_t *res)
{
    g_fake = f;
    const time_ready_sources_t src = {
        .now = now_cb,
        .seed_from_rtc = rtc_cb,
        .sync_from_network = sntp_cb,
    };
    return time_ready_evaluate(&src, timeout_ms, res);
}

/* ---- epoch policy ------------------------------------------------------ */

static void test_gate_only_for_tls(void)
{
    /* A LAN server over http:// never needs the gate. */
    CHECK(!time_ready_needed("http://tesserae.local:8765", ""));
    CHECK(!time_ready_needed("http://192.168.1.20:8765", NULL));
    CHECK(!time_ready_needed(NULL, NULL));
    CHECK(!time_ready_needed("", ""));
    /* Anything that will handshake does. */
    CHECK(time_ready_needed("https://cloud.tesserae.ink", ""));
    CHECK(time_ready_needed("HTTPS://cloud.tesserae.ink", ""));
    CHECK(time_ready_needed("", "https://relay.tesserae.ink"));
    CHECK(time_ready_needed("http://tesserae.local", "https://relay.tesserae.ink"));
    /* Prefixes that only look like https are not. */
    CHECK(!time_ready_needed("https:/x", ""));
    CHECK(!time_ready_needed("http", ""));
}

static void test_epoch_window(void)
{
    CHECK(!time_ready_epoch_ok(0));                       /* wiped clock */
    CHECK(!time_ready_epoch_ok(1704067200LL));            /* 2024-01-01 */
    CHECK(!time_ready_epoch_ok(TIME_READY_MIN_EPOCH - 1));
    CHECK(time_ready_epoch_ok(TIME_READY_MIN_EPOCH));     /* 2025-01-01 */
    CHECK(time_ready_epoch_ok(1767225600LL));             /* 2026-01-01 */
    CHECK(time_ready_epoch_ok(TIME_READY_MAX_EPOCH));
    CHECK(!time_ready_epoch_ok(TIME_READY_MAX_EPOCH + 1));
    CHECK(!time_ready_epoch_ok(-1));
}

/* ---- the combined condition (the H2E defect) --------------------------- */

static void test_clock_acceptable_window_and_floor(void)
{
    const clock_floor_t none = clock_floor_none();
    const clock_floor_t marked_2026_06 = { .present = true, .epoch = 1780272000LL };

    /* H2E B3: a rollback clock plus an SNTP reply from the year 2200. The
     * rollback branch used to check only the floor, which 2200 satisfies. */
    CHECK(!time_ready_clock_acceptable(7258118400LL, marked_2026_06));
    CHECK(!time_ready_clock_acceptable(7258118400LL, none));

    /* window edges, no mark */
    CHECK(!time_ready_clock_acceptable(TIME_READY_MIN_EPOCH - 1, none));
    CHECK(time_ready_clock_acceptable(TIME_READY_MIN_EPOCH, none));
    CHECK(time_ready_clock_acceptable(TIME_READY_MAX_EPOCH, none));
    CHECK(!time_ready_clock_acceptable(TIME_READY_MAX_EPOCH + 1, none));

    /* floor edges, plausible window */
    CHECK(time_ready_clock_acceptable(marked_2026_06.epoch, marked_2026_06));
    CHECK(time_ready_clock_acceptable(marked_2026_06.epoch - CLOCK_FLOOR_TOLERANCE_S, marked_2026_06));
    CHECK(!time_ready_clock_acceptable(marked_2026_06.epoch - CLOCK_FLOOR_TOLERANCE_S - 1, marked_2026_06));

    /* both failures at once, and plain junk */
    CHECK(!time_ready_clock_acceptable(0, marked_2026_06));
    CHECK(!time_ready_clock_acceptable(0, none));
    CHECK(!time_ready_clock_acceptable(946684800LL, none));   /* 2000 */
}

/* ---- the decision ------------------------------------------------------ */

static void test_already_plausible_touches_nothing(void)
{
    fake_t f;
    fresh(&f);
    f.now = 1767225600LL;
    time_ready_result_t res;
    CHECK(evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_ALREADY_OK);
    CHECK(f.rtc_calls == 0);      /* no I2C on the normal path */
    CHECK(f.sntp_calls == 0);     /* no radio either */
}

static void test_rtc_path(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_result = true;
    time_ready_result_t res;
    CHECK(evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_FROM_RTC);
    CHECK(f.rtc_calls == 1);
    CHECK(f.sntp_calls == 0);     /* the RTC was enough */
    CHECK(f.now == f.rtc_sets);
}

static void test_rtc_that_seeds_an_implausible_date_falls_through(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_sets = 946684800LL;     /* 2000-01-01: chip powers up here */
    f.sntp_result = true;
    time_ready_result_t res;
    CHECK(evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_FROM_SNTP);
    CHECK(f.rtc_calls == 1);
    CHECK(f.sntp_calls == 1);
}

static void test_sntp_path(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_result = false;
    f.sntp_result = true;
    time_ready_result_t res;
    CHECK(evaluate(&f, 5000, &res));
    CHECK(res == TIME_READY_FROM_SNTP);
    CHECK(f.last_timeout == 5000);   /* the caller's budget is honoured */
}

static void test_sntp_that_answers_with_1970_is_refused(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_result = false;
    f.sntp_result = true;
    f.sntp_sets = 0;                 /* a reply, not a clock */
    time_ready_result_t res;
    CHECK(!evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_NO_TIME);
}

static void test_sntp_far_future_is_refused(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_result = false;
    f.sntp_result = true;
    f.sntp_sets = TIME_READY_MAX_EPOCH + 86400;
    time_ready_result_t res;
    CHECK(!evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_NO_TIME);
}

static void test_no_time_at_all(void)
{
    fake_t f;
    fresh(&f);
    f.rtc_result = false;
    f.sntp_result = false;
    time_ready_result_t res;
    CHECK(!evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_NO_TIME);
    CHECK(f.rtc_calls == 1);
    CHECK(f.sntp_calls == 1);        /* exactly one attempt, not a loop */
}

static void test_rtc_voltage_loss_flag_means_ask_the_network(void)
{
    /* seed_clock() returns false when the VL bit is set; the network is the
     * next source, and if it works the wake proceeds. */
    fake_t f;
    fresh(&f);
    f.rtc_result = false;
    f.sntp_result = true;
    time_ready_result_t res;
    CHECK(evaluate(&f, 8000, &res));
    CHECK(res == TIME_READY_FROM_SNTP);
}

static void test_null_result_and_sources_are_safe(void)
{
    CHECK(!time_ready_evaluate(NULL, 1000, NULL));
    fake_t f;
    fresh(&f);
    g_fake = &f;
    const time_ready_sources_t no_sources = {0};
    CHECK(!time_ready_evaluate(&no_sources, 1000, NULL));
}

static void test_result_names_are_distinct(void)
{
    const char *a = time_ready_result_name(TIME_READY_ALREADY_OK);
    const char *b = time_ready_result_name(TIME_READY_FROM_RTC);
    const char *c = time_ready_result_name(TIME_READY_FROM_SNTP);
    const char *d = time_ready_result_name(TIME_READY_NO_TIME);
    CHECK(a && b && c && d);
    CHECK(strcmp(a, b) != 0 && strcmp(b, c) != 0 && strcmp(c, d) != 0);
}

int main(void)
{
    test_epoch_window();
    test_clock_acceptable_window_and_floor();
    test_already_plausible_touches_nothing();
    test_rtc_path();
    test_rtc_that_seeds_an_implausible_date_falls_through();
    test_sntp_path();
    test_sntp_that_answers_with_1970_is_refused();
    test_sntp_far_future_is_refused();
    test_no_time_at_all();
    test_rtc_voltage_loss_flag_means_ask_the_network();
    test_null_result_and_sources_are_safe();
    test_result_names_are_distinct();
    test_gate_only_for_tls();

    printf("time_ready: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
