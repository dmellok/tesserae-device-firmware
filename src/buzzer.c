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

/* Envelope the device enforces regardless of what the server sends. The
 * pattern plays on the input path, blocking it, so the ceiling is about how
 * long someone will stand with a finger on the glass; the frequency band is
 * what a small piezo actually reproduces. The server validates the same
 * bounds before storing, and neither side trusts the other. */
#define BUZZER_MAX_NOTES     8
#define BUZZER_MAX_TOTAL_MS  1000
#define BUZZER_NOTE_MAX_MS   500
#define BUZZER_FREQ_MIN_HZ   200
#define BUZZER_FREQ_MAX_HZ   8000

/* Played when the server sends nothing usable: still feedback, which is the
 * point of the feature, rather than a silent panel and a puzzled user. */
#define BUZZER_FALLBACK      "2000:60"

static bool s_ready;

/* Read one "freq:ms" note, returning the character after it, or NULL at the
 * end of the string / on malformed input. Values clamp rather than reject: a
 * note that is merely too long or too shrill should still sound. */
static const char *parse_note(const char *p, int *freq, int *ms)
{
    char *end = NULL;
    long f = strtol(p, &end, 10);
    if (end == p || *end != ':') return NULL;
    p = end + 1;
    long d = strtol(p, &end, 10);
    if (end == p) return NULL;

    if (f != 0) {
        if (f < BUZZER_FREQ_MIN_HZ) f = BUZZER_FREQ_MIN_HZ;
        if (f > BUZZER_FREQ_MAX_HZ) f = BUZZER_FREQ_MAX_HZ;
    }
    if (d < 1) d = 1;
    if (d > BUZZER_NOTE_MAX_MS) d = BUZZER_NOTE_MAX_MS;
    *freq = (int)f;
    *ms   = (int)d;

    while (*end == ' ') end++;
    if (*end == ',') return end + 1;
    return (*end == '\0') ? end : NULL;   /* trailing junk: stop cleanly */
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

void buzzer_play(const char *pattern, int volume)
{
    if (volume <= 0) return;
    if (volume > 100) volume = 100;
    int duty = BUZZER_DUTY_FULL * volume / 100;
    if (duty <= 0) return;

    const char *p = (pattern && pattern[0]) ? pattern : BUZZER_FALLBACK;
    const char *cursor = p;
    int played = 0, total_ms = 0;
    for (int n = 0; cursor && *cursor && n < BUZZER_MAX_NOTES; n++) {
        int freq = 0, ms = 0;
        cursor = parse_note(cursor, &freq, &ms);
        if (!cursor) break;                        /* malformed from here on */
        if (total_ms + ms > BUZZER_MAX_TOTAL_MS) break;
        total_ms += ms;
        if (freq == 0) {                           /* a rest */
            buzzer_duty(0);
            vTaskDelay(pdMS_TO_TICKS(ms));
            played++;
            continue;
        }
        if (buzzer_start(freq) != ESP_OK) {
            ESP_LOGW(TAG, "ledc setup failed; no sound this wake");
            return;
        }
        buzzer_duty(duty);
        vTaskDelay(pdMS_TO_TICKS(ms));
        played++;
    }
    buzzer_duty(0);
    if (!played && p != BUZZER_FALLBACK) {
        /* Nothing parsed. A tone the operator typed is worth a log line, then
         * fall back rather than leave the input unacknowledged. */
        ESP_LOGW(TAG, "unplayable tone \"%s\"; using the default", p);
        buzzer_play(BUZZER_FALLBACK, volume);
    }
}

void buzzer_feedback(void)
{
    const rest_config_t *c = rest_config_get();
    if (!c->beep_enabled) return;
    buzzer_play(c->beep_pattern, c->beep_volume);
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
