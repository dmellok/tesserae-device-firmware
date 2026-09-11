/* busy_sleep.c -- see busy_sleep.h. */

#include "busy_sleep.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#if SOC_USB_SERIAL_JTAG_SUPPORTED
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "busy_sleep";

#ifndef EPD_NAP_MIN_MS
#define EPD_NAP_MIN_MS      200   /* timer backstop floor; GPIO wake ends it early */
#endif
#define EPD_NAP_MAX_REJECTS 8

static bool     s_wifi_up, s_ble_up, s_disabled;
static uint32_t s_count, s_rejects;

void epd_light_sleep_set_wifi(bool up)
{
    if (up != s_wifi_up) ESP_LOGD(TAG, "wifi %s", up ? "up" : "down");
    s_wifi_up = up;
}

void epd_light_sleep_set_ble(bool up)
{
    if (up != s_ble_up) ESP_LOGD(TAG, "ble %s", up ? "up" : "down");
    s_ble_up = up;
}

bool epd_light_sleep_allowed(void)
{
#ifdef EPD_NO_LIGHT_SLEEP
    return false;
#else
    if (s_wifi_up || s_ble_up || s_disabled) return false;
#if SOC_USB_SERIAL_JTAG_SUPPORTED
    /* A host on the native USB console would lose the link mid-paint; keep
     * the serial log readable on the bench and save power in the field. */
    if (usb_serial_jtag_is_connected()) return false;
#endif
    return true;
#endif
}

uint32_t epd_light_sleep_count(void) { return s_count; }

static uint32_t since_ms(int64_t t0)
{
    int64_t d = (esp_timer_get_time() - t0) / 1000;
    return d < 1 ? 1 : (uint32_t)d;   /* never report 0: loops must progress */
}

uint32_t epd_busy_sleep(gpio_num_t pin, int ready_level, uint32_t ms)
{
    int64_t t0 = esp_timer_get_time();
    if (ms == 0) ms = 1;
    if (!epd_light_sleep_allowed()) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return since_ms(t0);
    }
    if (gpio_get_level(pin) == ready_level) return since_ms(t0);

    /* IDF's S2/S3 sleep GPIO workaround (CONFIG_ESP_SLEEP_GPIO_RESET_
     * WORKAROUND, on in our defaults) switches every pad to an isolated
     * input for the duration of a light sleep. That is right for deep sleep
     * but here the panel is mid-waveform on those very pins: CS, DC, RST and
     * the rail enables float, the controller aborts or resets, and BUSY reads
     * high off its pull-up so the wait ends early (bench E1002 2026-09-11:
     * a 29 s Spectra refresh "finished" in 11 s, plus a task-watchdog trip
     * from the resulting wake/sleep spin). Keep the pads in their active
     * configuration across light sleep; deep sleep still uses hold/RTC. */
    static bool s_pads_kept;
    if (!s_pads_kept) {
        esp_sleep_enable_gpio_switch(false);
        s_pads_kept = true;
    }

    /* Level-triggered so a BUSY that released between the read above and the
     * sleep entry wakes us immediately instead of after the timer. */
    if (gpio_wakeup_enable(pin, ready_level ? GPIO_INTR_HIGH_LEVEL
                                            : GPIO_INTR_LOW_LEVEL) != ESP_OK ||
        esp_sleep_enable_gpio_wakeup() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return since_ms(t0);
    }
    /* The timer is only a backstop; the BUSY level wake ends the nap the
     * moment the controller is ready. It must be LONGER than the S3's light
     * sleep entry + exit overhead (several ms with PSRAM and flash power-down)
     * or IDF rejects the request outright (ESP_ERR_SLEEP_TOO_SHORT_SLEEP_
     * DURATION) and a 10 ms poll loop degenerates into a busy spin that hits
     * the driver's iteration cap in seconds. Bench E1002 2026-09-11: PON
     * timed out exactly that way. */
    uint32_t nap_ms = ms < EPD_NAP_MIN_MS ? EPD_NAP_MIN_MS : ms;
    esp_sleep_enable_timer_wakeup((uint64_t)nap_ms * 1000ULL);
    esp_err_t r = esp_light_sleep_start();

    /* Leave nothing armed for the deep-sleep entry that follows the paint:
     * it configures its own timer and ext1 sources from scratch. */
    gpio_wakeup_disable(pin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

    if (r == ESP_OK) {
        s_count++;
        s_rejects = 0;
        /* One tick to the idle task so the task watchdog sees it between
         * naps; a nap is >= EPD_NAP_MIN_MS so this is noise. */
        vTaskDelay(1);
        return since_ms(t0);
    }
    /* Rejected (too short, or a source already pending): honour the
     * driver's poll interval the old way so the wait still progresses, and
     * stop trying after a run of rejects rather than paying the entry cost
     * on every poll. */
    /* ESP_ERR_SLEEP_REJECT means the pin reached the ready level between
     * our check and the sleep entry, i.e. the wait is over: an expected race
     * on panels that pulse BUSY between phases (SSD1677), not a fault. */
    if (s_rejects == 0 && r != ESP_ERR_SLEEP_REJECT)
        ESP_LOGW(TAG, "light sleep rejected: %s (falling back to delay)", esp_err_to_name(r));
    if (r == ESP_ERR_SLEEP_REJECT && gpio_get_level(pin) == ready_level) {
        s_rejects = 0;                 /* the wait ended, nothing to retry */
        return since_ms(t0);
    }
    if (++s_rejects >= EPD_NAP_MAX_REJECTS) {
        ESP_LOGW(TAG, "%d consecutive rejects; light sleep off for this boot", s_rejects);
        s_disabled = true;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    return since_ms(t0);
}
