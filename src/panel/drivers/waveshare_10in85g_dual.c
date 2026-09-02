/*
 * Waveshare 10.85-inch G (B/W/Y/R), ported from Waveshare's ESP32 demo.
 * The controller is split in two: every 1360px row is 340 bytes, with the
 * first/last 170 bytes sent to CS_M/CS_S respectively.
 *
 * Input uses the server's native bwry_4 format: 2bpp, four pixels per byte,
 * MSB pair leftmost, with 0=black, 1=white, 2=yellow and 3=red. The panel uses
 * the same codes, so display() transmits them without palette translation.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_WAVESHARE_10IN85G_DUAL)

#include "drivers/waveshare_10in85g_dual.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_10in85g";
static bool s_port_inited;

#define ROW_BYTES       (EPD_WIDTH / 4)
#define HALF_ROW_BYTES  (ROW_BYTES / 2)
#define CMD_DATA        0x10
#define CMD_REFRESH     0x12

/* The official ESP32-C3 reference firmware bit-bangs SPI. Preserve its
 * per-bit timing and MSB-first ordering instead of using the hardware SPI
 * peripheral: this particular dual-controller adapter is sensitive to it. */
static esp_err_t tx(const uint8_t *data, size_t len)
{
    while (len--) {
        uint8_t value = *data++;
        for (int bit = 0; bit < 8; bit++) {
            gpio_set_level(EPD_PIN_MOSI, (value & 0x80) != 0);
            value <<= 1;
            gpio_set_level(EPD_PIN_SCLK, 1);
            gpio_set_level(EPD_PIN_SCLK, 0);
        }
    }
    return ESP_OK;
}

static void cs_all(int level)
{
    gpio_set_level(EPD_PIN_CS_M, level);
    gpio_set_level(EPD_PIN_CS_S, level);
}

static void command_all(uint8_t command)
{
    gpio_set_level(EPD_PIN_DC, 0); cs_all(0); tx(&command, 1); cs_all(1);
}

static void data_all(uint8_t value)
{
    gpio_set_level(EPD_PIN_DC, 1); cs_all(0); tx(&value, 1); cs_all(1);
}

static void command_for(gpio_num_t cs, uint8_t command)
{
    gpio_set_level(EPD_PIN_DC, 0); gpio_set_level(cs, 0); tx(&command, 1); gpio_set_level(cs, 1);
}

static void data_for(gpio_num_t cs, const uint8_t *data, size_t len)
{
    /* Exact Waveshare DEV_SPI_WriteByte framing: CS is pulsed for each byte. */
    gpio_set_level(EPD_PIN_DC, 1);
    while (len--) {
        gpio_set_level(cs, 0);
        tx(data++, 1);
        gpio_set_level(cs, 1);
    }
}

/* BUSY is active-low. A normal refresh is slow; 90s is a safe fail-stop. */
static bool wait_idle(void)
{
    for (int elapsed = 0; elapsed < 90000; elapsed += 10) {
        if (gpio_get_level(EPD_PIN_BUSY)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "BUSY remained low for 90 seconds");
    return false;
}

static void reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t port_init(void)
{
    if (s_port_inited) return ESP_OK;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) |
                        (1ULL << EPD_PIN_CS_M) | (1ULL << EPD_PIN_CS_S) |
                        (1ULL << EPD_PIN_PWR) | (1ULL << EPD_PIN_SCLK) |
                        (1ULL << EPD_PIN_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    gpio_config_t in = {.pin_bit_mask = 1ULL << EPD_PIN_BUSY, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&in);
    gpio_set_level(EPD_PIN_PWR, 0); cs_all(1); gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_MOSI, 0);
    s_port_inited = true;
    return ESP_OK;
}

static void init(void)
{
    gpio_set_level(EPD_PIN_PWR, 1); vTaskDelay(pdMS_TO_TICKS(20));
    reset(); wait_idle();
    /* Exact register sequence from Waveshare EPD_10in85g.cpp. */
    command_all(0x4D); data_all(0x78);
    command_all(0xE0); data_all(0x01);
    command_all(0xE5); data_all(0x08);
    command_all(0xA2); data_all(0x01); command_all(0x00); data_all(0x2F); data_all(0x21);
    command_all(0xA2); data_all(0x02); command_all(0x00); data_all(0x2F); data_all(0x21);
    command_all(0xA2); data_all(0x00);
    command_all(0x01); data_all(0x07); data_all(0x00);
    command_all(0x06); const uint8_t boost[] = {0x0D,0x12,0x30,0x20,0x19,0x3D,0x0C};
    for (size_t i = 0; i < sizeof boost; i++) data_all(boost[i]);
    command_all(0x30); data_all(0x08); command_all(0x50); data_all(0x37);
    command_all(0x61); data_all(0x02); data_all(0xA8); data_all(0x01); data_all(0xE0);
    command_all(0x65); data_all(0); data_all(0); data_all(0); data_all(0);
    command_all(0xE3); data_all(0x88); command_all(0xE9); data_all(0x01);
    command_all(0xB8); data_all(0xB5); vTaskDelay(pdMS_TO_TICKS(200));
    command_all(0x04); vTaskDelay(pdMS_TO_TICKS(500)); wait_idle();
}

static void refresh(void)
{
    command_all(CMD_REFRESH); data_all(0); wait_idle();
}

static void display(const uint8_t *image)
{
    command_for(EPD_PIN_CS_M, CMD_DATA);
    for (int y = 0; y < EPD_HEIGHT; y++) data_for(EPD_PIN_CS_M, image + y * ROW_BYTES, HALF_ROW_BYTES);
    command_for(EPD_PIN_CS_S, CMD_DATA);
    for (int y = 0; y < EPD_HEIGHT; y++) data_for(EPD_PIN_CS_S, image + y * ROW_BYTES + HALF_ROW_BYTES, HALF_ROW_BYTES);
    refresh();
}

static void clear(uint8_t color)
{
    uint8_t row[HALF_ROW_BYTES];
    memset(row, (uint8_t)((color << 6) | (color << 4) | (color << 2) | color), sizeof row);
    command_for(EPD_PIN_CS_M, CMD_DATA); for (int y = 0; y < EPD_HEIGHT; y++) data_for(EPD_PIN_CS_M, row, sizeof row);
    command_for(EPD_PIN_CS_S, CMD_DATA); for (int y = 0; y < EPD_HEIGHT; y++) data_for(EPD_PIN_CS_S, row, sizeof row);
    refresh();
}

static void bars(void)
{
    /* Match the vendor bring-up sequence: first erase shipping/ghost image,
     * then reset/init once more before painting the diagnostic palette. */
    clear(EPD_COL_WHITE);
    /* Keep this visible during bring-up: it distinguishes a bad white clear
     * from a bad subsequently streamed test frame. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    init();

    uint8_t *frame = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE(TAG, "not enough RAM for the 163200-byte self-test frame");
        return;
    }
    for (int y = 0; y < EPD_HEIGHT; y++) memset(frame + y * ROW_BYTES,
        (uint8_t)(((y * 4 / EPD_HEIGHT) << 6) | ((y * 4 / EPD_HEIGHT) << 4) | ((y * 4 / EPD_HEIGHT) << 2) | (y * 4 / EPD_HEIGHT)), ROW_BYTES);
    display(frame); free(frame);
}

static void sleep(void)
{
    command_all(0x02); data_all(0); vTaskDelay(pdMS_TO_TICKS(100));
    command_all(0x07); data_all(0xA5); gpio_set_level(EPD_PIN_PWR, 0);
}

const epd_driver_t waveshare_10in85g_dual_driver = {
    .info = {"Waveshare 10.85-inch G", EPD_WIDTH, EPD_HEIGHT, 2, EPD_BUF_BYTES, false},
    .port_init = port_init, .init = init, .clear = clear, .display = display,
    .show_color_bars = bars, .show_palette_sweep = bars, .sleep = sleep,
    .display_partial = NULL, .display_partial_mode = NULL,
};
#endif
