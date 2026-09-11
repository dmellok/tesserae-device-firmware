/* led.c -- single status LED. See led.h. */

#include "led.h"

#ifdef BOARD_LED_PIN

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef BOARD_LED_ACTIVE_LOW
#define BOARD_LED_ACTIVE_LOW 0
#endif

static const char *TAG = "led";

static esp_timer_handle_t s_timer;
static uint64_t s_on_us;        /* blink phase lengths */
static uint64_t s_off_us;
static bool     s_lit;          /* current blink phase */
static bool     s_ready;        /* led_init() ran */

static inline void led_drive(bool on)
{
    gpio_set_level((gpio_num_t)BOARD_LED_PIN, BOARD_LED_ACTIVE_LOW ? !on : on);
}

/* One-shot timer re-armed each phase, so on and off can differ in length
 * without a second timer. */
static void led_blink_cb(void *arg)
{
    (void)arg;
    s_lit = !s_lit;
    led_drive(s_lit);
    esp_timer_start_once(s_timer, s_lit ? s_on_us : s_off_us);
}

static void led_blink_stop(void)
{
    if (s_timer != NULL) esp_timer_stop(s_timer);   /* no-op if idle */
}

void led_init(void)
{
    if (s_ready) return;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LED_PIN,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    /* A boot after led_prepare_sleep() may find the pad still held; release it
     * before reconfiguring or the level below never reaches the pin. */
    gpio_hold_dis((gpio_num_t)BOARD_LED_PIN);
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGW(TAG, "GPIO%d config failed", BOARD_LED_PIN);
        return;
    }
    s_ready = true;
    led_drive(true);
}

void led_set(bool on)
{
    if (!s_ready) led_init();
    if (!s_ready) return;
    led_blink_stop();
    led_drive(on);
}

void led_blink(uint32_t on_ms, uint32_t period_ms)
{
    if (!s_ready) led_init();
    if (!s_ready) return;

    led_blink_stop();
    if (period_ms == 0) {
        led_drive(false);
        return;
    }
    if (on_ms > period_ms) on_ms = period_ms;

    if (s_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = led_blink_cb,
            .name     = "led_blink",
        };
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            ESP_LOGW(TAG, "timer create failed");
            led_drive(true);
            return;
        }
    }

    s_on_us  = (uint64_t)on_ms * 1000ULL;
    s_off_us = (uint64_t)(period_ms - on_ms) * 1000ULL;

    /* Start lit. A zero-length phase would re-arm the timer at 0 us, so fall
     * back to steady on/off for the degenerate duty cycles. */
    if (s_off_us == 0) { led_drive(true);  return; }
    if (s_on_us  == 0) { led_drive(false); return; }
    s_lit = true;
    led_drive(true);
    esp_timer_start_once(s_timer, s_on_us);
}

void led_prepare_sleep(void)
{
    led_blink_stop();
    if (!s_ready) return;
    led_drive(false);

#if BOARD_LED_ACTIVE_LOW
    /* Active-low: the LED hangs between 3V3 and the pin, so an input with no
     * pulls sinks nothing and is off. That draws exactly as much as a held-high
     * output (zero) without needing gpio_deep_sleep_hold_en(), which is global
     * and which the E1003 touch path has a documented reason to avoid. */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_LED_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#else
    /* Active-high: LED to ground. Hold the pad low through the pad isolation so
     * it cannot float up and glow on its way into sleep. Every ESP32-S3 GPIO
     * supports gpio_hold_en(), so no validity check is needed here. */
    gpio_hold_en((gpio_num_t)BOARD_LED_PIN);
#endif
    s_ready = false;   /* led_init() reconfigures on the next boot / wake */
}

#endif /* BOARD_LED_PIN */
