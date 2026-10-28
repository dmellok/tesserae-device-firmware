/*
 * Sitronix ST7305 reflective LCD, 400x300, 1bpp (Waveshare ESP32-S3-RLCD-4.2).
 *
 * Transport: 4-wire SPI (SCLK, MOSI, CS, DC) plus RST. No BUSY line: the
 * controller accepts a new frame whenever it is awake, and a write of the full
 * frame memory takes about 12 ms at 10 MHz.
 *
 * Frame contract: the shared Tesserae 1bpp frame -- EPD_WIDTH x EPD_HEIGHT
 * row-major, 8 px/byte MSB = leftmost, bit 1 = white -- exactly as splash.c
 * draws it and as the esp32_bw_bin renderer packs it. The controller wants
 * something else: its memory runs down the panel's long side 300 "columns" at
 * a time, 2 frame columns x 4 frame rows per byte, with the frame's row axis
 * reversed. display() transposes into that layout in a DMA-capable scratch
 * buffer, so the rest of the firmware never sees it (same split as the Sticky's
 * transposed ssd1677 path).
 *
 * The init bytes are Waveshare's (display_bsp.cpp, RLCD_Init) verbatim: gate and
 * source voltages, oscillator, the two gate-EQ tables, 300 gate lines, 1-bit
 * colour mode, inversion on, address window 0x12..0x2A x 0x00..0xC7. Do not
 * "tidy" them -- they are the glass's electrical setup.
 *
 * Power: an RLCD shows an image only while the controller drives it. Sleep-in
 * (0x10) stops the drive and the panel goes blank, so sleep() does NOT send it.
 * It leaves the controller in low-power mode (0x39, the ~1 Hz internal
 * refresh the datasheet specifies at tens of microamps); the board header
 * holds RST and CS high and SCLK/MOSI/DC low through the SoC's deep sleep
 * (BOARD_SLEEP_DRIVE_*_MASK, applied by main.c on every sleep, painted or
 * not). The next wake that paints sees the panel still alive (an RTC-memory
 * flag plus the reset reason), skips the hardware reset, re-sends the init
 * sequence (harmless on a running controller, and it repairs one that lost
 * its registers) and writes the frame. The old image stays up until the new
 * bytes land: no blank, no flash.
 *
 * Verified on a Waveshare ESP32-S3-RLCD-4.2 on 2026-10-27: selftest pattern
 * upright, dashboard frames over REST, image held through low-power mode and
 * deep sleep, battery and SHTC3 readings.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_ST7305_RLCD)

#include "drivers/st7305_rlcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rlcd_st7305";

/* The address window and gate-line count below are this glass's. A second
 * ST7305 panel wants its own numbers, so refuse to build for other geometry
 * rather than drive it with these. */
_Static_assert(EPD_WIDTH == 400 && EPD_HEIGHT == 300,
               "st7305_rlcd carries the Waveshare 4.2\" window; other glass needs its own");
_Static_assert(EPD_BUF_BYTES == (EPD_WIDTH * EPD_HEIGHT) / 8,
               "st7305_rlcd expects a packed 1bpp frame");

/* ---------- ST7305 command set (datasheet V0.2) ---------- */
#define CMD_SLPIN    0x10
#define CMD_SLPOUT   0x11
#define CMD_INVON    0x21
#define CMD_DISPOFF  0x28
#define CMD_DISPON   0x29
#define CMD_CASET    0x2A
#define CMD_RASET    0x2B
#define CMD_RAMWR    0x2C
#define CMD_TEON     0x35
#define CMD_MADCTL   0x36
#define CMD_HPM      0x38   /* high power mode: ~51 Hz refresh, for the write */
#define CMD_LPM      0x39   /* low power mode: ~1 Hz refresh, holds the image */
#define CMD_COLMOD   0x3A

/* Address window for the 400x300 glass (Waveshare). Columns 0x12..0x2A =
 * 25 units of 12 px = 300; rows 0x00..0xC7 = 200 units of 2 px = 400. */
static const uint8_t CASET_V[] = {0x12, 0x2A};
static const uint8_t RASET_V[] = {0x00, 0xC7};

/* ---------- controller memory layout ----------
 * The controller's "column" axis runs along the frame's SHORT side (300) and
 * its "row" axis along the LONG side (400). One byte holds 2 controller rows
 * x 4 controller columns. In frame terms, with c = (H-1-y) and r = x:
 *   index = (r >> 1) * (H/4) + (c >> 2)
 *   bit   = 7 - (((c & 3) << 1) | (r & 1))
 * i.e. the frame's two pixels at (2k, y) and (2k+1, y) land in one byte, in
 * adjacent bits, at a bit position chosen by y. Lifted from Waveshare's
 * InitLandscapeLUT(); the ESPHome port of the same board agrees. */
#define NATIVE_BLOCKS_PER_COL (EPD_HEIGHT / 4)   /* 75 */
#define FRAME_ROW_BYTES       (EPD_WIDTH / 8)    /* 50 */

static esp_lcd_panel_io_handle_t s_io = NULL;   /* esp_lcd SPI panel IO, hardware CS */
static bool     s_port_inited = false;
static uint8_t *s_native = NULL;      /* EPD_BUF_BYTES, internal DMA memory */

/* True once sleep() parked the controller in low-power mode (its pins are
 * then held by the board masks). Lives in RTC memory so a deep-sleep wake (and a software reset) can
 * tell that the image is still up and skip the hardware reset. A power-on
 * reset zeroes it. */
static RTC_DATA_ATTR bool s_panel_live = false;

/* ---------- low-level SPI/GPIO ----------
 * The transport is ESP-IDF's esp_lcd SPI panel IO, the same path Waveshare's
 * own driver uses: hardware chip select, DC toggled by the driver inside each
 * transaction, mode 0. A hand-rolled spi_master transport with a GPIO chip
 * select (the shape every e-paper driver here uses) left this glass blank on
 * the bench while the vendor's firmware drew on it; whatever the difference
 * was, matching their path removed it. */

/* One CS-framed command with optional trailing parameter bytes. Waits for
 * any frame write still in flight first (esp_lcd drains its queue). */
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "cmd 0x%02x: %s", cmd, esp_err_to_name(err));
}

#define CMD(c)        cmd_data((c), NULL, 0)
#define CMD_V(c, ...) do { static const uint8_t v_[] = { __VA_ARGS__ }; \
                           cmd_data((c), v_, sizeof v_); } while (0)

static void hw_reset(void)
{
    /* Waveshare's timing: high 50 ms, low 20 ms, high 50 ms. */
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---------- frame transpose ---------- */

/* Pack a Tesserae row-major 1bpp frame into the controller's layout. */
static void transpose_frame(const uint8_t *frame, uint8_t *native)
{
    memset(native, 0, EPD_BUF_BYTES);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const int c       = EPD_HEIGHT - 1 - y;
        const int block_y = c >> 2;
        const int shift   = 6 - ((c & 3) << 1);   /* pair lands at bits shift+1, shift */
        const uint8_t *row = frame + (size_t)y * FRAME_ROW_BYTES;
        for (int sx = 0; sx < FRAME_ROW_BYTES; sx++) {
            const uint8_t src = row[sx];
            /* Four pixel pairs per source byte: pair k = frame x 8sx+2k, +1,
             * i.e. controller row pair (byte_x) = 4sx + k. The pair's high bit
             * is the even x (r & 1 == 0 -> bit 7-2*(c&3)), low bit the odd. */
            uint8_t *col = native + ((size_t)sx * 4) * NATIVE_BLOCKS_PER_COL + block_y;
            col[0 * NATIVE_BLOCKS_PER_COL] |= (uint8_t)(((src >> 6) & 3) << shift);
            col[1 * NATIVE_BLOCKS_PER_COL] |= (uint8_t)(((src >> 4) & 3) << shift);
            col[2 * NATIVE_BLOCKS_PER_COL] |= (uint8_t)(((src >> 2) & 3) << shift);
            col[3 * NATIVE_BLOCKS_PER_COL] |= (uint8_t)(((src >> 0) & 3) << shift);
        }
    }
}

/* Write the whole native buffer to the controller's frame memory. The vendor
 * sends RAMWR as its own transaction and the bytes as a colour transfer after
 * it; a CS release in between is fine, the write pointer survives it. The
 * colour transfer is queued; the next command (there is always one: HPM/LPM
 * before the next write or the sleep) waits for it. */
static void write_native(void)
{
    cmd_data(CMD_CASET, CASET_V, sizeof CASET_V);
    cmd_data(CMD_RASET, RASET_V, sizeof RASET_V);
    CMD(CMD_RAMWR);
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, -1, s_native, EPD_BUF_BYTES);
    if (err != ESP_OK) ESP_LOGE(TAG, "frame write: %s", esp_err_to_name(err));
}

/* ---------- driver entry points ---------- */

static esp_err_t st7305_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    /* sleep() held these across the deep sleep. Release them before the SPI
     * peripheral claims them: a hold left on pins them at their parked level
     * and the first transaction clocks nothing. RST is configured and driven
     * high first so it never dips on release. */
    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_RST),
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_hold_dis((gpio_num_t)EPD_PIN_RST);
    gpio_hold_dis((gpio_num_t)EPD_PIN_CS);
    gpio_hold_dis((gpio_num_t)EPD_PIN_DC);
    gpio_hold_dis((gpio_num_t)EPD_PIN_SCLK);
    gpio_hold_dis((gpio_num_t)EPD_PIN_MOSI);

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_PIN_MOSI,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_BYTES,
    };
    esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(bus_err);
    }
    esp_lcd_panel_io_spi_config_t io = {
        .dc_gpio_num = EPD_PIN_DC,
        .cs_gpio_num = EPD_PIN_CS,
        .pclk_hz = EPD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EPD_SPI_HOST, &io, &s_io));

    if (!s_native) {
        s_native = heap_caps_malloc(EPD_BUF_BYTES,
                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_native) {
            ESP_LOGE(TAG, "no internal memory for the %u-byte native frame",
                     (unsigned)EPD_BUF_BYTES);
            return ESP_ERR_NO_MEM;
        }
    }

    s_port_inited = true;
    return ESP_OK;
}

/* Is the controller still running the image we left? Only after a deep-sleep
 * wake or a software restart can the flag be trusted: anything else (power-on,
 * brown-out, panic) may have dropped the panel rail with the SoC. */
static bool panel_still_live(void)
{
    if (!s_panel_live) return false;
    esp_reset_reason_t r = esp_reset_reason();
    return r == ESP_RST_DEEPSLEEP || r == ESP_RST_SW;
}

static void st7305_init(void)
{
    if (panel_still_live()) {
        ESP_LOGI(TAG, "panel still live from the last sleep; no hardware reset");
    } else {
        hw_reset();
    }
    s_panel_live = false;

    /* Waveshare RLCD_Init(), byte for byte. */
    CMD_V(0xD6, 0x17, 0x02);                 /* NVM load control */
    CMD_V(0xD1, 0x01);                       /* booster enable */
    CMD_V(0xC0, 0x11, 0x04);                 /* gate voltage VGH / VGL */
    CMD_V(0xC1, 0x69, 0x69, 0x69, 0x69);     /* VSHP (source high, HPM) */
    CMD_V(0xC2, 0x19, 0x19, 0x19, 0x19);     /* VSLP (source low, HPM) */
    CMD_V(0xC4, 0x4B, 0x4B, 0x4B, 0x4B);     /* VSHN */
    CMD_V(0xC5, 0x19, 0x19, 0x19, 0x19);     /* VSLN */
    CMD_V(0xD8, 0x80, 0xE9);                 /* oscillator */
    CMD_V(0xB2, 0x02);                       /* frame rate */
    CMD_V(0xB3, 0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45);  /* gate EQ, HPM */
    CMD_V(0xB4, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45);              /* gate EQ, LPM */
    CMD_V(0x62, 0x32, 0x03, 0x1F);           /* gate timing */
    CMD_V(0xB7, 0x13);                       /* source EQ enable */
    CMD_V(0xB0, 0x64);                       /* gate lines: 0x64 * 3 = 300 */
    CMD(CMD_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(200));          /* datasheet: 120 ms minimum after sleep-out */
    CMD_V(0xC9, 0x00);                       /* source voltage select: set 1 */
    CMD_V(CMD_MADCTL, 0x48);                 /* MX=1, DO=1 */
    CMD_V(CMD_COLMOD, 0x11);                 /* 1 bit per pixel */
    CMD_V(0xB9, 0x20);                       /* gamma mode: monochrome */
    CMD_V(0xB8, 0x29);                       /* panel setting: 1-dot + frame inversion, interlace */
    CMD(CMD_INVON);                          /* bit 1 = white with this polarity */
    cmd_data(CMD_CASET, CASET_V, sizeof CASET_V);
    cmd_data(CMD_RASET, RASET_V, sizeof RASET_V);
    CMD_V(CMD_TEON, 0x00);                   /* tearing-effect line on (TE pin unused here) */
    CMD_V(0xD0, 0xFF);                       /* auto power-down control */
    CMD(CMD_HPM);                            /* high power mode for the write */
    CMD(CMD_DISPON);
}

static void st7305_clear(uint8_t color)
{
    memset(s_native, color ? 0xFF : 0x00, EPD_BUF_BYTES);
    CMD(CMD_HPM);
    write_native();
}

static void st7305_display(const uint8_t *image)
{
    transpose_frame(image, s_native);
    CMD(CMD_HPM);
    CMD(CMD_DISPON);
    write_native();
}

/* Diagnostic: eight horizontal bands (black at the top, then alternating),
 * a solid black square in the top-left corner and a black diagonal from the
 * top-left to the bottom-right. The bands prove the transport; the square and
 * the diagonal prove the transpose: square at top-left with the line falling
 * to the right is correct, a mirrored or rotated figure means the layout
 * above does not match this glass. Built as an ordinary frame so it goes
 * through the same path the server's frames do. */
static void st7305_show_color_bars(void)
{
    uint8_t *fb = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_8BIT);
    if (!fb) { ESP_LOGE(TAG, "no memory for the selftest frame"); return; }
    const int BAND_H = EPD_HEIGHT / 8;   /* 37 rows; the last band takes the remainder */
    for (int y = 0; y < EPD_HEIGHT; y++) {
        int band = y / BAND_H; if (band > 7) band = 7;
        memset(fb + (size_t)y * FRAME_ROW_BYTES, (band & 1) ? 0xFF : 0x00, FRAME_ROW_BYTES);
    }
    /* Top band is black, so paint the square and the line into WHITE where
     * they cross a black band and black elsewhere -- always visible. */
    for (int y = 0; y < EPD_HEIGHT; y++) {
        int x0 = y * EPD_WIDTH / EPD_HEIGHT;
        for (int x = x0; x < x0 + 3 && x < EPD_WIDTH; x++) {
            uint8_t *b = fb + (size_t)y * FRAME_ROW_BYTES + (x >> 3);
            *b ^= (uint8_t)(0x80 >> (x & 7));
        }
        if (y < 40) {
            for (int x = 0; x < 40; x++) {
                uint8_t *b = fb + (size_t)y * FRAME_ROW_BYTES + (x >> 3);
                *b ^= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    st7305_display(fb);
    free(fb);
}

/* Diagnostic: alternating single pixels, a finer transport check. */
static void st7305_show_palette_sweep(void)
{
    uint8_t *fb = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_8BIT);
    if (!fb) return;
    for (int y = 0; y < EPD_HEIGHT; y++)
        memset(fb + (size_t)y * FRAME_ROW_BYTES, (y & 1) ? 0x55 : 0xAA, FRAME_ROW_BYTES);
    st7305_display(fb);
    free(fb);
}

static void st7305_sleep(void)
{
    /* Keep the image: low-power mode, display on. Sleep-in would blank it.
     * The pins are held through the deep sleep by the board's
     * BOARD_SLEEP_DRIVE_HIGH_MASK / _LOW_MASK (main.c sleep_park_rails),
     * which run on EVERY sleep, including the wakes that never paint and so
     * never reach this driver. */
    CMD(CMD_LPM);
    s_panel_live = true;
}

/* ---------- exported vtable ---------- */

const epd_driver_t st7305_rlcd_driver = {
    .info = {
        .name      = "RLCD 4.2\" ST7305 (400x300, 1bpp)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 1,
        .buf_bytes = EPD_BUF_BYTES,
        .grayscale = false,
    },
    .port_init          = st7305_port_init,
    .init               = st7305_init,
    .clear              = st7305_clear,
    .display            = st7305_display,
    .display_gray       = NULL,
    .show_color_bars    = st7305_show_color_bars,
    .show_palette_sweep = st7305_show_palette_sweep,
    .sleep              = st7305_sleep,
    .display_partial    = NULL,
};

#endif /* PANEL_DRIVER_ST7305_RLCD */
