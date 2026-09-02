/*
 * Waveshare 10.85-inch G (B/W/Y/R), ported from Waveshare's ESP32 demo.
 * The controller is split in two: every 1360px row is 340 bytes, with the
 * first/last 170 bytes sent to CS_M/CS_S respectively.
 *
 * Input uses the server's native bwry_4 format: 2bpp, four pixels per byte,
 * MSB pair leftmost, with 0=black, 1=white, 2=yellow and 3=red. The panel uses
 * the same codes, so ws1085g_display() transmits them without palette translation.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_WAVESHARE_1085G_DUAL)

#include "drivers/waveshare_1085g_dual.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_1085g";
static bool s_port_inited;

#define ROW_BYTES       (EPD_WIDTH / 4)
#define HALF_ROW_BYTES  (ROW_BYTES / 2)
#define CMD_DATA        0x10
#define CMD_REFRESH     0x12

/* The official ESP32-C3 reference firmware bit-bangs SPI. Preserve its
 * per-bit timing and MSB-first ordering instead of using the hardware SPI
 * peripheral: this particular dual-controller adapter is sensitive to it. */
static esp_err_t ws1085g_tx(const uint8_t *data, size_t len)
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

static void ws1085g_cs_all(int level)
{
    gpio_set_level(EPD_PIN_CS_M, level);
    gpio_set_level(EPD_PIN_CS_S, level);
}

static void ws1085g_command_all(uint8_t command)
{
    gpio_set_level(EPD_PIN_DC, 0); ws1085g_cs_all(0); ws1085g_tx(&command, 1); ws1085g_cs_all(1);
}

static void ws1085g_data_all(uint8_t value)
{
    gpio_set_level(EPD_PIN_DC, 1); ws1085g_cs_all(0); ws1085g_tx(&value, 1); ws1085g_cs_all(1);
}

static void ws1085g_command_for(gpio_num_t cs, uint8_t command)
{
    gpio_set_level(EPD_PIN_DC, 0); gpio_set_level(cs, 0); ws1085g_tx(&command, 1); gpio_set_level(cs, 1);
}

static void ws1085g_data_for(gpio_num_t cs, const uint8_t *data, size_t len)
{
    /* Exact Waveshare DEV_SPI_WriteByte framing: CS is pulsed for each byte. */
    gpio_set_level(EPD_PIN_DC, 1);
    while (len--) {
        gpio_set_level(cs, 0);
        ws1085g_tx(data++, 1);
        gpio_set_level(cs, 1);
    }
}

/* BUSY is active-low. A normal refresh is slow; 90s is a safe fail-stop. */
static bool ws1085g_wait_idle(void)
{
    for (int elapsed = 0; elapsed < 90000; elapsed += 10) {
        if (gpio_get_level(EPD_PIN_BUSY)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "BUSY remained low for 90 seconds");
    return false;
}

static void ws1085g_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

static esp_err_t ws1085g_port_init(void)
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
    gpio_set_level(EPD_PIN_PWR, 0); ws1085g_cs_all(1); gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_MOSI, 0);
    s_port_inited = true;
    return ESP_OK;
}

static void ws1085g_init(void)
{
    gpio_set_level(EPD_PIN_PWR, 1); vTaskDelay(pdMS_TO_TICKS(20));
    ws1085g_reset(); ws1085g_wait_idle();
    /* Exact register sequence from Waveshare EPD_10in85g.cpp. */
    ws1085g_command_all(0x4D); ws1085g_data_all(0x78);
    ws1085g_command_all(0xE0); ws1085g_data_all(0x01);
    ws1085g_command_all(0xE5); ws1085g_data_all(0x08);
    ws1085g_command_all(0xA2); ws1085g_data_all(0x01); ws1085g_command_all(0x00); ws1085g_data_all(0x2F); ws1085g_data_all(0x21);
    ws1085g_command_all(0xA2); ws1085g_data_all(0x02); ws1085g_command_all(0x00); ws1085g_data_all(0x2F); ws1085g_data_all(0x21);
    ws1085g_command_all(0xA2); ws1085g_data_all(0x00);
    ws1085g_command_all(0x01); ws1085g_data_all(0x07); ws1085g_data_all(0x00);
    ws1085g_command_all(0x06); const uint8_t boost[] = {0x0D,0x12,0x30,0x20,0x19,0x3D,0x0C};
    for (size_t i = 0; i < sizeof boost; i++) ws1085g_data_all(boost[i]);
    ws1085g_command_all(0x30); ws1085g_data_all(0x08); ws1085g_command_all(0x50); ws1085g_data_all(0x37);
    ws1085g_command_all(0x61); ws1085g_data_all(0x02); ws1085g_data_all(0xA8); ws1085g_data_all(0x01); ws1085g_data_all(0xE0);
    ws1085g_command_all(0x65); ws1085g_data_all(0); ws1085g_data_all(0); ws1085g_data_all(0); ws1085g_data_all(0);
    ws1085g_command_all(0xE3); ws1085g_data_all(0x88); ws1085g_command_all(0xE9); ws1085g_data_all(0x01);
    ws1085g_command_all(0xB8); ws1085g_data_all(0xB5); vTaskDelay(pdMS_TO_TICKS(200));
    ws1085g_command_all(0x04); vTaskDelay(pdMS_TO_TICKS(500)); ws1085g_wait_idle();
}

static void ws1085g_refresh(void)
{
    ws1085g_command_all(CMD_REFRESH); ws1085g_data_all(0); ws1085g_wait_idle();
}

static void ws1085g_display(const uint8_t *image)
{
    ws1085g_command_for(EPD_PIN_CS_M, CMD_DATA);
    for (int y = 0; y < EPD_HEIGHT; y++) ws1085g_data_for(EPD_PIN_CS_M, image + y * ROW_BYTES, HALF_ROW_BYTES);
    ws1085g_command_for(EPD_PIN_CS_S, CMD_DATA);
    for (int y = 0; y < EPD_HEIGHT; y++) ws1085g_data_for(EPD_PIN_CS_S, image + y * ROW_BYTES + HALF_ROW_BYTES, HALF_ROW_BYTES);
    ws1085g_refresh();
}

static void ws1085g_clear(uint8_t color)
{
    uint8_t row[HALF_ROW_BYTES];
    memset(row, (uint8_t)((color << 6) | (color << 4) | (color << 2) | color), sizeof row);
    ws1085g_command_for(EPD_PIN_CS_M, CMD_DATA); for (int y = 0; y < EPD_HEIGHT; y++) ws1085g_data_for(EPD_PIN_CS_M, row, sizeof row);
    ws1085g_command_for(EPD_PIN_CS_S, CMD_DATA); for (int y = 0; y < EPD_HEIGHT; y++) ws1085g_data_for(EPD_PIN_CS_S, row, sizeof row);
    ws1085g_refresh();
}

static void ws1085g_bars(void)
{
    /* Match the vendor bring-up sequence: first erase shipping/ghost image,
     * then reset/init once more before painting the diagnostic palette. */
    ws1085g_clear(EPD_COL_WHITE);
    /* Keep this visible during bring-up: it distinguishes a bad white clear
     * from a bad subsequently streamed test frame. */
    vTaskDelay(pdMS_TO_TICKS(3000));
    ws1085g_init();

    uint8_t *frame = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE(TAG, "not enough RAM for the 163200-byte self-test frame");
        return;
    }
    for (int y = 0; y < EPD_HEIGHT; y++) memset(frame + y * ROW_BYTES,
        (uint8_t)(((y * 4 / EPD_HEIGHT) << 6) | ((y * 4 / EPD_HEIGHT) << 4) | ((y * 4 / EPD_HEIGHT) << 2) | (y * 4 / EPD_HEIGHT)), ROW_BYTES);
    ws1085g_display(frame); free(frame);
}

static void ws1085g_sleep(void)
{
    ws1085g_command_all(0x02); ws1085g_data_all(0); vTaskDelay(pdMS_TO_TICKS(100));
    ws1085g_command_all(0x07); ws1085g_data_all(0xA5); gpio_set_level(EPD_PIN_PWR, 0);
}

const epd_driver_t waveshare_1085g_dual_driver = {
    .info = {"Waveshare 10.85-inch G", EPD_WIDTH, EPD_HEIGHT, 2, EPD_BUF_BYTES, false},
    .port_init = ws1085g_port_init, .init = ws1085g_init, .clear = ws1085g_clear, .display = ws1085g_display,
    .show_color_bars = ws1085g_bars, .show_palette_sweep = ws1085g_bars, .sleep = ws1085g_sleep,
    .display_partial = NULL, .display_partial_mode = NULL,
};
#endif
