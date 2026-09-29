/*
 * diag_run.c: RTC-backed detected-failure latch. See diag_run.h.
 */
#include "diag_run.h"

#include <time.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "diag";

RTC_NOINIT_ATTR static diag_state_t s_diag;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_booted;

/* Same plausibility window main.c uses for the wall clock. */
static uint32_t epoch_now(void)
{
    time_t t = time(NULL);
    return (t > 1700000000 && t < 2200000000LL) ? (uint32_t)t : 0;
}

static diag_reset_t reset_kind(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_BROWNOUT: return DIAG_RESET_BROWNOUT;
    case ESP_RST_PANIC:    return DIAG_RESET_PANIC;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return DIAG_RESET_WATCHDOG;
    default:               return DIAG_RESET_NONE;
    }
}

void diag_run_boot(void)
{
    if (s_booted) return;
    /* Random start, like the button event ids: the server dedups on id, and a
     * power-on must not replay ids it has already seen. Top bit clear so the
     * counter has room before it wraps. */
    uint32_t seed = esp_random() >> 1;
    uint32_t now = epoch_now();
    diag_reset_t rk = reset_kind(esp_reset_reason());
    diag_report_t rep;
    portENTER_CRITICAL(&s_mux);
    diag_boot(&s_diag, rk, seed, now);
    bool pending = diag_get(&s_diag, &rep);
    s_booted = true;
    portEXIT_CRITICAL(&s_mux);
    if (pending) {
        const char *pe = diag_paint_name(rep.paint);
        const char *rs = diag_reset_name(rep.reset);
        ESP_LOGW(TAG, "failure report %lu waiting: paint_error=%s reset=%s",
                 (unsigned long)rep.id, pe ? pe : "-", rs ? rs : "-");
    }
}

void diag_note_paint(diag_paint_t paint)
{
    if (!s_booted) diag_run_boot();
    uint32_t now = epoch_now();
    portENTER_CRITICAL(&s_mux);
    bool changed = diag_latch_paint(&s_diag, paint, now);
    uint32_t id = s_diag.id;
    portEXIT_CRITICAL(&s_mux);
    if (changed)
        ESP_LOGW(TAG, "latched %s (report %lu)", diag_paint_name(paint),
                 (unsigned long)id);
}

bool diag_run_report(diag_report_t *out)
{
    if (!s_booted) diag_run_boot();
    portENTER_CRITICAL(&s_mux);
    bool pending = diag_get(&s_diag, out);
    portEXIT_CRITICAL(&s_mux);
    return pending;
}

void diag_run_clear(uint32_t id)
{
    portENTER_CRITICAL(&s_mux);
    diag_clear(&s_diag, id);
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "failure report %lu delivered", (unsigned long)id);
}
