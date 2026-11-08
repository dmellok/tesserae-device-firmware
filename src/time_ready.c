#include "time_ready.h"

#include <time.h>

const char *time_ready_result_name(time_ready_result_t result)
{
    switch (result) {
    case TIME_READY_ALREADY_OK: return "already-ok";
    case TIME_READY_FROM_RTC:   return "rtc";
    case TIME_READY_FROM_SNTP:  return "sntp";
    case TIME_READY_NO_TIME:    return "none";
    }
    return "unknown";
}

bool time_ready_epoch_ok(int64_t epoch)
{
    return epoch >= TIME_READY_MIN_EPOCH && epoch <= TIME_READY_MAX_EPOCH;
}

bool time_ready_evaluate(const time_ready_sources_t *sources, int timeout_ms,
                         time_ready_result_t *result)
{
    time_ready_result_t unused = TIME_READY_NO_TIME;
    if (!result) result = &unused;
    *result = TIME_READY_NO_TIME;
    if (!sources || !sources->now) return false;

    /* Already plausible: leave it alone. This is the normal path after the
     * boot-time RTC seed, and it costs neither an I2C transaction nor a radio. */
    if (time_ready_epoch_ok(sources->now())) {
        *result = TIME_READY_ALREADY_OK;
        return true;
    }

    /* The RTC seeded something, but "it returned true" only means the write
     * succeeded -- the value itself still has to be plausible before it can be
     * trusted. A chip that reports 2000 is not a clock. */
    if (sources->seed_from_rtc && sources->seed_from_rtc()) {
        if (time_ready_epoch_ok(sources->now())) {
            *result = TIME_READY_FROM_RTC;
            return true;
        }
    }

    /* One bounded network sync. Its result is judged the same way: a reply is
     * not a clock until it is inside the permitted window. */
    if (sources->sync_from_network && sources->sync_from_network(timeout_ms)) {
        if (time_ready_epoch_ok(sources->now())) {
            *result = TIME_READY_FROM_SNTP;
            return true;
        }
    }

    *result = TIME_READY_NO_TIME;
    return false;
}

#ifdef ESP_PLATFORM
/* ---- firmware glue: the same decision, with the real sources ---------- */

#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "rtc_pcf8563.h"

static const char *TAG = "time_ready";

/* Initialised once per boot; deep sleep clears it with the rest of RAM. */
static bool s_sntp_started;

static bool sntp_sync(int timeout_ms)
{
    if (!s_sntp_started) {
        /* One server, because CONFIG_LWIP_SNTP_MAX_SERVERS defaults to 1.
         * Ordinary SNTP is not authenticated; see the note in the PR body and
         * the residual-risk register in the qualification report. */
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "sntp init failed: %s", esp_err_to_name(err));
            return false;
        }
        s_sntp_started = true;
    }
    if (timeout_ms <= 0) timeout_ms = TIME_READY_TIMEOUT_MS;
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sntp sync did not complete: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int64_t system_now(void)
{
    return (int64_t)time(NULL);
}

bool time_ready_ensure(int timeout_ms, time_ready_result_t *result)
{
    const time_ready_sources_t sources = {
        .now = system_now,
        .seed_from_rtc = rtc_pcf8563_seed_clock,
        .sync_from_network = sntp_sync,
    };
    time_ready_result_t got = TIME_READY_NO_TIME;
    const bool ok = time_ready_evaluate(&sources, timeout_ms, &got);
    if (result) *result = got;
    if (ok) {
        ESP_LOGI(TAG, "clock ready (%s): %lld", time_ready_result_name(got),
                 (long long)system_now());
    } else {
        ESP_LOGW(TAG, "no trustworthy clock after rtc+%dms sntp; "
                      "skipping TLS work this wake", timeout_ms);
    }
    return ok;
}
#else
/* Host build: the glue needs ESP-IDF and real hardware, so it is not compiled.
 * time_ready_evaluate() above is what the host tests exercise. */
bool time_ready_ensure(int timeout_ms, time_ready_result_t *result)
{
    (void)timeout_ms;
    if (result) *result = TIME_READY_NO_TIME;
    return false;
}
#endif
