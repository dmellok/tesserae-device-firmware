/* buzzer.c -- passive piezo feedback (server #258). See buzzer.h. */

#include "buzzer.h"

#ifdef BOARD_BUZZER_PIN

#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rest_config.h"

static const char *TAG = "buzzer";

/* LEDC low-speed channel 0. Nothing else on these boards uses LEDC (no
 * backlight on e-ink), so the channel is ours for the life of the wake. */
#define BUZZER_TIMER      LEDC_TIMER_0
#define BUZZER_CHANNEL    LEDC_CHANNEL_0
#define BUZZER_MODE       LEDC_LOW_SPEED_MODE
/* 10-bit duty resolution: plenty for a volume percentage, and it keeps the
 * timer's clock divider comfortable at every tone frequency below. */
#define BUZZER_DUTY_BITS  LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX   1023

/* A square wave delivers most power to the piezo at 50% duty; past that it is
 * symmetric and gets quieter again. So 100% volume means half the range, and
 * the scale below is the audible part of it. */
#define BUZZER_DUTY_FULL  (BUZZER_DUTY_MAX / 2)

/* One tone is up to two steps, so a chirp can rise without needing a
 * sequencer. Frequencies sit in the 1.2-2.6 kHz band where a small piezo is
 * loudest; a step with freq 0 is unused. */
typedef struct {
    const char *name;
    struct { int freq; int ms; } step[2];
} buzzer_tone_t;

static const buzzer_tone_t TONES[] = {
    { "click", { { 2400, 12 }, { 0, 0 } } },
    { "beep",  { { 2000, 60 }, { 0, 0 } } },
    { "chirp", { { 1800, 30 }, { 2600, 40 } } },
    { "low",   { { 1200, 60 }, { 0, 0 } } },
};
#define TONE_DEFAULT 1   /* "beep" */

static bool s_ready;

static const buzzer_tone_t *tone_by_name(const char *name)
{
    if (name && name[0]) {
        for (size_t i = 0; i < sizeof TONES / sizeof TONES[0]; i++)
            if (strcmp(TONES[i].name, name) == 0) return &TONES[i];
    }
    return &TONES[TONE_DEFAULT];
}

static esp_err_t buzzer_start(int freq)
{
    ledc_timer_config_t timer = {
        .speed_mode      = BUZZER_MODE,
        .duty_resolution = BUZZER_DUTY_BITS,
        .timer_num       = BUZZER_TIMER,
        .freq_hz         = freq,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    if (s_ready) return ESP_OK;

    ledc_channel_config_t ch = {
        .gpio_num   = BOARD_BUZZER_PIN,
        .speed_mode = BUZZER_MODE,
        .channel    = BUZZER_CHANNEL,
        .timer_sel  = BUZZER_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err == ESP_OK) s_ready = true;
    return err;
}

static void buzzer_duty(int duty)
{
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
}

void buzzer_play(const char *tone_name, int volume)
{
    if (volume <= 0) return;
    if (volume > 100) volume = 100;
    int duty = BUZZER_DUTY_FULL * volume / 100;
    if (duty <= 0) return;

    const buzzer_tone_t *tone = tone_by_name(tone_name);
    for (size_t i = 0; i < sizeof tone->step / sizeof tone->step[0]; i++) {
        int freq = tone->step[i].freq, ms = tone->step[i].ms;
        if (freq <= 0 || ms <= 0) break;
        if (buzzer_start(freq) != ESP_OK) {
            ESP_LOGW(TAG, "ledc setup failed; no sound this wake");
            return;
        }
        buzzer_duty(duty);
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    buzzer_duty(0);
}

void buzzer_feedback(void)
{
    const rest_config_t *c = rest_config_get();
    if (!c->beep_enabled) return;
    buzzer_play(c->beep_tone, c->beep_volume);
}

void buzzer_idle(void)
{
    if (!s_ready) return;
    buzzer_duty(0);
    ledc_stop(BUZZER_MODE, BUZZER_CHANNEL, 0);
    s_ready = false;
    /* Park the pin low rather than leaving it floating: a floating gate on a
     * piezo driver can sit at a level that idles it audibly. */
    gpio_reset_pin(BOARD_BUZZER_PIN);
    gpio_set_direction(BOARD_BUZZER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_BUZZER_PIN, 0);
}

#endif /* BOARD_BUZZER_PIN */
