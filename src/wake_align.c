/* See wake_align.h for the design notes. */
#include "wake_align.h"

#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"

static const char *TAG = "wake_align";

/* Same "is the wall clock plausible" bounds the rest of the firmware
 * uses (main.c EPOCH_REASONABLE_*): ~2023..2100. */
#define EPOCH_SANE_MIN  1672531200U   /* 2023-01-01 */
#define EPOCH_SANE_MAX  4102444800U   /* 2100-01-01 */

/* One-shot absolute wake target from the latest /status response. Plain
 * RAM: resets to 0 on every wake, so a stale target can never outlive
 * the sleep it was issued for. */
static uint32_t s_target_epoch = 0;

/* Drift bookkeeping, in RTC RAM so it survives deep sleep. s_drift_ppm
 * is positive when the local clock runs SLOW against the server (the
 * usual internal-RC case): local time falls behind, and a timer sleep
 * measured on the same oscillator runs long by the same ratio. */
RTC_DATA_ATTR static int64_t s_last_disc_ms = 0;
RTC_DATA_ATTR static float   s_drift_ppm   = 0.0f;
RTC_DATA_ATTR static bool    s_drift_valid = false;

#define DRIFT_EWMA_ALPHA       0.3f
/* Baselines shorter than this can't separate drift from the Date
 * header's whole-second granularity + network latency. Same-wake
 * disciplines (a few seconds apart) only advance the baseline. */
#define DRIFT_MIN_BASELINE_MS  60000
/* An adjustment this large over one cycle is an outage, a reboot with a
 * dead clock, or a server clock jump -- not oscillator drift. */
#define DRIFT_MAX_ERR_MS       120000
/* The internal RC drifts low single-digit percent at the very worst. */
#define DRIFT_MAX_PPM          60000.0f

void wake_align_set_target(uint32_t epoch)
{
    s_target_epoch = epoch;
}

int wake_align_sleep_s(int fallback_s)
{
    if (s_target_epoch == 0 || fallback_s <= 0) return fallback_s;
    time_t now = time(NULL);
    if (now < (time_t)EPOCH_SANE_MIN || now > (time_t)EPOCH_SANE_MAX) return fallback_s;
    int64_t delta = (int64_t)s_target_epoch - (int64_t)now;
    /* Target already passed (or as good as): the relative fallback is
     * the safe answer, not a zero-length sleep. */
    if (delta < 5) return fallback_s;
    /* The target and next_poll_s encode the same instant, so delta can
     * only be SHORTER than the fallback (time has passed since the
     * response). A fallback shorter than delta means another arbiter
     * (collection playback) wants an earlier wake; it wins. */
    if (delta > (int64_t)fallback_s + 2) return fallback_s;
    ESP_LOGI(TAG, "sleeping to absolute target: %ld s (relative would be %d s)",
             (long)delta, fallback_s);
    return (int)delta;
}

void wake_align_note_discipline(int64_t local_ms, int64_t server_ms)
{
    if (server_ms <= 0) return;
    if (s_last_disc_ms > 0) {
        int64_t baseline = server_ms - s_last_disc_ms;
        int64_t err      = server_ms - local_ms;
        if (baseline >= DRIFT_MIN_BASELINE_MS &&
            err > -DRIFT_MAX_ERR_MS && err < DRIFT_MAX_ERR_MS &&
            local_ms > (int64_t)EPOCH_SANE_MIN * 1000) {
            float ppm = (float)err * 1e6f / (float)baseline;
            if (ppm > -DRIFT_MAX_PPM && ppm < DRIFT_MAX_PPM) {
                s_drift_ppm = s_drift_valid
                                  ? (1.0f - DRIFT_EWMA_ALPHA) * s_drift_ppm
                                        + DRIFT_EWMA_ALPHA * ppm
                                  : ppm;
                s_drift_valid = true;
                ESP_LOGI(TAG, "clock drift: %+.0f ppm this cycle, ewma %+.0f ppm",
                         (double)ppm, (double)s_drift_ppm);
            }
        }
    }
    s_last_disc_ms = server_ms;
}

uint64_t wake_align_timer_us(int sleep_s)
{
    uint64_t us = (uint64_t)sleep_s * 1000000ULL;
    if (!s_drift_valid) return us;
    /* Local clock slow (positive ppm) => the timer runs long => program
     * proportionally fewer microseconds, clamped to the plausible band. */
    float factor = 1.0f - s_drift_ppm / 1e6f;
    if (factor < 0.94f) factor = 0.94f;
    if (factor > 1.06f) factor = 1.06f;
    return (uint64_t)((double)us * (double)factor);
}
