/*
 * Seeed reTerminal Sticky -- 3.97" 800x480 4-level grayscale, SSD1677. See
 * ssd1677_gray.h for why this is its own family rather than a mono_spi variant.
 *
 * Sequences ported from Seeed_GFX TFT_Drivers/SSD1677_Defines.h (EPD_INIT_GRAY,
 * EPD_UPDATE_GRAY, EPD_PUSH_NEW_GRAY_COLORS). Transport mirrors mono_spi: a DC
 * command/data line, one CS driven by hand, chunked SPI writes.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_SSD1677_GRAY)

#include "drivers/ssd1677_gray.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_ssd1677";

/* SSD1677 opcodes. Nothing here is shared with the UC8179 panels. */
#define SSD_SWRESET     0x12
#define SSD_BOOSTER     0x0c
#define SSD_BORDER      0x3c
#define SSD_DRV_OUTPUT  0x01
#define SSD_DATA_ENTRY  0x11
#define SSD_RAM_X       0x44
#define SSD_RAM_Y       0x45
#define SSD_TEMP_SENSOR 0x18
#define SSD_TEMP_WRITE  0x1a
#define SSD_RAM_X_ADDR  0x4e
#define SSD_RAM_Y_ADDR  0x4f
#define SSD_DTM1        0x24   /* first plane  */
#define SSD_DTM2        0x26   /* second plane */
#define SSD_UPD_CTRL    0x22
#define SSD_TRIGGER     0x20
#define SSD_SLEEP       0x10

/* Update-control byte for a full 4-gray refresh (Seeed's EPD_UPDATE_GRAY).
 * The mono path uses 0xF7; 0xD7 is the grayscale variant, which drives the
 * OTP waveform table rather than the black/white one. */
#define SSD_UPD_GRAY    0xd7

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* ---------- low-level SPI/GPIO ---------- */

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    const size_t CHUNK = 4096;
    spi_transaction_t t;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
        memset(&t, 0, sizeof t);
        t.length = n * 8;
        t.tx_buffer = data + off;
        esp_err_t err = spi_device_polling_transmit(s_spi, &t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi tx fail at off=%u: %s", (unsigned)off,
                     esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static void send_cmd(uint8_t cmd)
{
    gpio_set_level(EPD_PIN_DC, 0);
    spi_tx_raw(&cmd, 1);
}

static void send_data(const uint8_t *buf, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    spi_tx_raw(buf, len);
}

static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(cmd);
    if (len) send_data(data, len);
    gpio_set_level(EPD_PIN_CS, 1);
}

/* BUSY is active HIGH on this controller -- the opposite of every UC8179 panel
 * in this tree. Getting this backwards does not fail loudly: the wait returns
 * immediately, the caller powers the panel down mid-waveform, and the result is
 * a partial or blank frame that looks like a bad LUT.
 *
 * Bounded for the same reason mono_spi bounds its wait: a panel whose BUSY
 * never clears must still reach its power-down, because continuing with a bad
 * frame costs one refresh while hanging with the panel driven costs the image. */
#define SSD_BUSY_TIMEOUT_MS 45000

static bool wait_idle(void)
{
    int waited = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY)) {      /* HIGH = busy */
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (!warned && waited >= 10000) {
            ESP_LOGW(TAG, "BUSY still high after 10 s; panel may be stuck");
            warned = true;
        }
        if (waited >= SSD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY never cleared in %d ms; continuing so the panel "
                          "still gets powered down", SSD_BUSY_TIMEOUT_MS);
            return false;
        }
    }
    return true;
}

static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---------- 4-gray plane encoding ----------
 *
 * Two planes, each 1bpp, 8 px/byte MSB-first. From Seeed's push macro, with the
 * frame's 2bpp values as 0 = black .. 3 = white:
 *
 *        white  light-gray  dark-gray  black        (g = 3    2    1    0)
 *   0x24:  0        0           1        1          (bit = ((g >> 1) & 1) ^ 1)
 *   0x26:  0        1           0        1          (bit =  (g & 1)      ^ 1)
 *
 * Both planes are inverted with respect to the level, so white is (0,0) and
 * black is (1,1). Kept as macros for the same reason mono_spi does: the clear
 * and the test pattern must derive their fills from the SAME table as the image
 * path, or they drift apart silently. */
#define GRAY_P1_BIT(g) ((((g) >> 1) & 1) ^ 1)   /* 0x24 */
#define GRAY_P2_BIT(g) (((g) & 1) ^ 1)          /* 0x26 */

/* Stream one 1bpp plane derived from the 2bpp frame. */
static void gray_send_plane(uint8_t plane_cmd, const uint8_t *image, int plane)
{
    uint8_t row[EPD_WIDTH / 8];
    const uint8_t *in = image;

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(plane_cmd);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        for (int b = 0; b < EPD_WIDTH / 8; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 2; k++) {          /* 2 input bytes -> 8 px */
                uint8_t v = *in++;
                for (int p = 0; p < 4; p++) {
                    uint8_t g = (uint8_t)((v >> (6 - 2 * p)) & 0x3);
                    uint8_t bit = (plane == 1) ? GRAY_P1_BIT(g) : GRAY_P2_BIT(g);
                    o = (uint8_t)((o << 1) | (bit & 1));
                }
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Emit `rows` solid plane rows (caller has sent the plane command, CS low). */
static void gray_send_solid_rows(uint8_t fill, int rows)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, fill, sizeof row);
    for (int y = 0; y < rows; y++) send_data(row, sizeof row);
}

/* ---------- driver entry points ---------- */

static esp_err_t ssd1677_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) |
                        (1ULL << EPD_PIN_CS)
#ifdef EPD_PIN_EN
                      | (1ULL << EPD_PIN_EN)
#endif
        ,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_RST, 1);

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_PIN_MOSI,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_BYTES,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,            /* we drive CS by hand */
        .queue_size = 1,
    };
    /* The SD card shares this bus on the Sticky and may have claimed it first;
     * tolerate INVALID_STATE the same way mono_spi does. */
    esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(bus_err);
    }
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

static void ssd1677_init(void)
{
#ifdef EPD_PIN_EN
    gpio_set_level(EPD_PIN_EN, 1);    /* panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    hw_reset();
    wait_idle();

    cmd_data(SSD_SWRESET, NULL, 0);
    wait_idle();

    static const uint8_t BOOSTER[] = {0xae, 0xc7, 0xc3, 0xc0, 0x80};
    cmd_data(SSD_BOOSTER, BOOSTER, sizeof BOOSTER);

    /* Border. 0x00 on the grayscale path (Seeed uses 0x05 for mono); the border
     * follows the waveform rather than being held at a fixed level. */
    static const uint8_t BORDER[] = {0x00};
    cmd_data(SSD_BORDER, BORDER, sizeof BORDER);

    /* Driver output: gate count is HEIGHT-1, little-endian, then a mode byte. */
    const uint8_t drv[] = {(uint8_t)((EPD_HEIGHT - 1) & 0xff),
                           (uint8_t)((EPD_HEIGHT - 1) >> 8), 0x02};
    cmd_data(SSD_DRV_OUTPUT, drv, sizeof drv);

    static const uint8_t ENTRY[] = {0x03};       /* X inc, Y inc */
    cmd_data(SSD_DATA_ENTRY, ENTRY, sizeof ENTRY);

    const uint8_t xwin[] = {0x00, 0x00,
                            (uint8_t)((EPD_WIDTH - 1) & 0xff),
                            (uint8_t)((EPD_WIDTH - 1) >> 8)};
    cmd_data(SSD_RAM_X, xwin, sizeof xwin);
    const uint8_t ywin[] = {0x00, 0x00,
                            (uint8_t)((EPD_HEIGHT - 1) & 0xff),
                            (uint8_t)((EPD_HEIGHT - 1) >> 8)};
    cmd_data(SSD_RAM_Y, ywin, sizeof ywin);

    static const uint8_t TSENS[] = {0x80};       /* internal temperature sensor */
    cmd_data(SSD_TEMP_SENSOR, TSENS, sizeof TSENS);

    /* Fixed temperature for the grayscale waveform. The 4-gray OTP table is
     * selected by a written temperature rather than the measured one, exactly
     * like TSSET on the UC8179 gen2 path -- so this is waveform selection, not
     * compensation, and must not be swapped for a live sensor reading. */
    static const uint8_t TWRITE[] = {0x67, 0x00};
    cmd_data(SSD_TEMP_WRITE, TWRITE, sizeof TWRITE);

    static const uint8_t ZERO2[] = {0x00, 0x00};
    cmd_data(SSD_RAM_X_ADDR, ZERO2, sizeof ZERO2);
    cmd_data(SSD_RAM_Y_ADDR, ZERO2, sizeof ZERO2);
    wait_idle();

    ESP_LOGI(TAG, "init complete (SSD1677 4-gray, %dx%d)", EPD_WIDTH, EPD_HEIGHT);
}

/* Refresh an already-loaded frame. */
static void trigger_refresh(void)
{
    static const uint8_t UPD[] = {SSD_UPD_GRAY};
    int64_t t0 = esp_timer_get_time();
    cmd_data(SSD_UPD_CTRL, UPD, sizeof UPD);
    cmd_data(SSD_TRIGGER, NULL, 0);
    wait_idle();
    ESP_LOGI(TAG, "refresh done (%lld ms)",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void ssd1677_display(const uint8_t *image)
{
    gray_send_plane(SSD_DTM1, image, 1);
    gray_send_plane(SSD_DTM2, image, 2);
    trigger_refresh();
}

static void ssd1677_clear(uint8_t color)
{
    uint8_t g  = (uint8_t)(color & 0x3);
    uint8_t p1 = GRAY_P1_BIT(g) ? 0xFF : 0x00;
    uint8_t p2 = GRAY_P2_BIT(g) ? 0xFF : 0x00;

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    gray_send_solid_rows(p1, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    gray_send_solid_rows(p2, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}

/* Bring-up: 4 horizontal bands, black -> dark -> light -> white. Even spacing
 * between the two mid greys is the thing to judge; if the ends are swapped the
 * plane encoding above is inverted for this glass. */
static void ssd1677_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 4;

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows(GRAY_P1_BIT(g) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows(GRAY_P2_BIT(g) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}

/* Transport check: alternating pixels on both planes. */
static void ssd1677_show_palette_sweep(void)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, 0xAA, sizeof row);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int y = 0; y < EPD_HEIGHT; y++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    for (int y = 0; y < EPD_HEIGHT; y++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}

static void ssd1677_sleep(void)
{
    static const uint8_t DEEP[] = {0x01};
    cmd_data(SSD_SLEEP, DEEP, sizeof DEEP);
    vTaskDelay(pdMS_TO_TICKS(100));
#ifdef EPD_PIN_EN
    gpio_set_level(EPD_PIN_EN, 0);    /* cut panel power */
#endif
}

/* ---------- exported vtable ---------- */

const epd_driver_t ssd1677_gray_driver = {
    .info = {
        .name      = "SSD1677 3.97\" (800x480, 4-gray 2bpp)",
        .bpp       = 2,
        .grayscale = true,
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = ssd1677_port_init,
    .init               = ssd1677_init,
    .clear              = ssd1677_clear,
    .display            = ssd1677_display,
    .show_color_bars    = ssd1677_show_color_bars,
    .show_palette_sweep = ssd1677_show_palette_sweep,
    .sleep              = ssd1677_sleep,
};

#endif /* PANEL_DRIVER_SSD1677_GRAY */
