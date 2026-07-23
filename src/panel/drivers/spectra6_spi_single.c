/*
 * Seeed reTerminal E1002 -- single-controller Spectra-6 (800x480), Family B.
 *
 * Ported from a confirmed-working ESP-IDF Spectra-6 update transport for the
 * BOARD_SEEED_RETERMINAL_E1002 spectra6_* path -- a direct ESP-IDF SPI update
 * confirmed working on real E1002 hardware. The reference is already ESP-IDF
 * SPI/GPIO; only the Arduino glue
 * (the delay, logging, and framebuffer accessor) is translated here. The
 * init/refresh register values and the transport quirks (command via the
 * SPI command phase;
 * 128-byte, CS-framed data chunks) are reproduced exactly.
 *
 * Split across the epd_driver_t vtable: port_init = spectra6_init_spi; init =
 * reset + the init command block; display = DTM + framebuffer + PON/DRF/POF;
 * sleep = deep sleep. main.c calls them consecutively, so the emitted sequence
 * matches the reference's monolithic spectra6_update().
 *
 * Shared with the Waveshare PhotoPainter 7.3" (same ED2208-GCA init, byte for
 * byte). Two board macros tailor it there:
 *   - EPD_ROTATE_180: reverse the byte stream + swap nibbles on the full-frame
 *     send (the PhotoPainter mounts the panel 180 degrees in its case).
 *   - BOARD_HAS_PMIC: panel power is an AXP2101 LDO rail, so port_init/init
 *     bring the rails up via pmic_*() instead of relying on a GPIO gate.
 *   - EPD_PIN_PWR: boards with a GPIO panel-power gate (EE04) define it; the
 *     driver raises it before init and cuts it on sleep, like the dual driver.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_SPECTRA6_SPI_SINGLE)

#include "drivers/spectra6_spi_single.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef BOARD_HAS_PMIC
#include "pmic.h"                 /* AXP2101 panel rails (PhotoPainter) */
#endif

static const char *TAG = "epd_s6single";

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* Block while BUSY is low (panel busy); UC81xx idle == BUSY high. ~60 s cap. */
static bool wait_busy(const char *label)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    uint32_t n = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++n > 6000) {
            ESP_LOGE(TAG, "%s BUSY timeout", label);
            return false;
        }
    }
    return true;
}

static bool spi_tx(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(s_spi, &t) == ESP_OK;
}

/* One command, framed by CS, with the opcode on the SPI command phase (DC low)
 * and optional data bytes after (DC high). Matches spectra6_command_data(). */
static bool cmd_data(uint8_t command, const uint8_t *data, size_t len)
{
    if (!s_spi) return false;

    spi_device_acquire_bus(s_spi, portMAX_DELAY);
    gpio_set_level(EPD_PIN_DC, 0);
    gpio_set_level(EPD_PIN_CS, 0);

    spi_transaction_ext_t ct = {0};
    ct.command_bits = 8;
    ct.base.flags = SPI_TRANS_VARIABLE_CMD;
    ct.base.cmd = command;
    esp_err_t err = spi_device_polling_transmit(s_spi, &ct.base);

    if (err == ESP_OK && data && len) {
        gpio_set_level(EPD_PIN_DC, 1);
        err = spi_tx(data, len) ? ESP_OK : ESP_FAIL;
    }

    gpio_set_level(EPD_PIN_CS, 1);
    spi_device_release_bus(s_spi);
    return err == ESP_OK;
}

static bool cmd(uint8_t command) { return cmd_data(command, NULL, 0); }

/* Stream frame data in 128-byte, CS-framed chunks (DC high). Reproduces
 * spectra6_send_buffer() exactly, including the per-chunk CS toggle. */
static bool send_buffer(const uint8_t *data, size_t len)
{
    if (!s_spi) return false;
    const uint8_t *cur = data;
    size_t remaining = len;
    while (remaining) {
        const size_t chunk = remaining > 128 ? 128 : remaining;
        spi_device_acquire_bus(s_spi, portMAX_DELAY);
        gpio_set_level(EPD_PIN_DC, 1);
        gpio_set_level(EPD_PIN_CS, 0);
        const bool ok = spi_tx(cur, chunk);
        gpio_set_level(EPD_PIN_CS, 1);
        spi_device_release_bus(s_spi);
        if (!ok) return false;
        cur += chunk;
        remaining -= chunk;
    }
    return true;
}

#ifdef EPD_ROTATE_180
/* Stream a full frame rotated 180 degrees: panel byte i is pulled from source
 * byte (len-1-i) (reverses row + column-pair order in one shot on this flat
 * scanline-major buffer) and each byte's nibbles are swapped (flips the two
 * pixels packed in it). Copies each chunk through a stack-local buffer, which
 * also keeps the SPI DMA off a PSRAM source. Matches the PhotoPainter port. */
static bool send_buffer_rot180(const uint8_t *data, size_t len)
{
    if (!s_spi) return false;
    uint8_t local[128];
    const uint8_t *src_end = data + len;
    size_t remaining = len;
    while (remaining) {
        const size_t chunk = remaining > 128 ? 128 : remaining;
        const uint8_t *src = src_end - chunk;
        for (size_t i = 0; i < chunk; i++) {
            uint8_t b = src[chunk - 1 - i];
            local[i] = (uint8_t)((b << 4) | (b >> 4));
        }
        spi_device_acquire_bus(s_spi, portMAX_DELAY);
        gpio_set_level(EPD_PIN_DC, 1);
        gpio_set_level(EPD_PIN_CS, 0);
        const bool ok = spi_tx(local, chunk);
        gpio_set_level(EPD_PIN_CS, 1);
        spi_device_release_bus(s_spi);
        if (!ok) return false;
        src_end   -= chunk;
        remaining -= chunk;
    }
    return true;
}
#endif /* EPD_ROTATE_180 */

static void reset_panel(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---------- driver entry points ---------- */

static esp_err_t s6s_port_init(void)
{
    if (s_port_inited) return ESP_OK;

#ifdef BOARD_HAS_PMIC
    /* Panel power is an AXP2101 rail on this board -- bring it up (and the I2C
     * bus) before we touch RST/CS. Idempotent; battery.c may have run it
     * already. Non-fatal if the PMIC is MIA (logged inside). */
    pmic_init();
    pmic_rails_set(true);
#endif

    gpio_config_t out = {0};
    out.pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_CS);
#ifdef EPD_PIN_PWR
    out.pin_bit_mask |= (1ULL << EPD_PIN_PWR);
#endif
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {0};
    in.pin_bit_mask = (1ULL << EPD_PIN_BUSY);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 0);
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);
#endif

    spi_bus_config_t bus = {0};
    bus.mosi_io_num = EPD_PIN_MOSI;
    bus.miso_io_num = -1;
    bus.sclk_io_num = EPD_PIN_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;

    esp_err_t err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev = {0};
    dev.clock_speed_hz = EPD_SPI_HZ;
    dev.mode = 0;
    dev.spics_io_num = -1;            /* we drive CS by hand */
    dev.queue_size = 1;
    dev.flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY;

    err = spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        s_spi = NULL;
        return err;
    }

    s_port_inited = true;
    ESP_LOGI(TAG, "SPI initialized");
    return ESP_OK;
}

/* Canned init parameter blobs (from the reference init; do NOT edit). */
static const uint8_t CMD_H[] = {0x49, 0x55, 0x20, 0x08, 0x09, 0x18};
static const uint8_t PWR[]   = {0x3f};
static const uint8_t PSR[]   = {0x5f, 0x69};
static const uint8_t PFS[]   = {0x00, 0x54, 0x00, 0x44};
static const uint8_t BTST1[] = {0x40, 0x1f, 0x1f, 0x2c};
static const uint8_t BTST2[] = {0x6f, 0x1f, 0x17, 0x49};
static const uint8_t BTST3[] = {0x6f, 0x1f, 0x1f, 0x22};
static const uint8_t PLL[]   = {0x03};
static const uint8_t CDI[]   = {0x3f};
static const uint8_t TCON[]  = {0x02, 0x00};
static const uint8_t TRES[]  = {0x03, 0x20, 0x01, 0xe0};   /* 0x0320=800 x 0x01e0=480 */
static const uint8_t TVDCS[] = {0x01};
static const uint8_t PWS[]   = {0x2f};

static bool run_init_sequence(void)
{
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 1);   /* EN: enable panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    reset_panel();
    if (!wait_busy("reset")) return false;

    if (!cmd_data(0xaa, CMD_H, sizeof CMD_H)) return false;
    if (!cmd_data(0x01, PWR,   sizeof PWR))   return false;
    if (!cmd_data(0x00, PSR,   sizeof PSR))   return false;
    if (!cmd_data(0x03, PFS,   sizeof PFS))   return false;
    if (!cmd_data(0x05, BTST1, sizeof BTST1)) return false;
    if (!cmd_data(0x06, BTST2, sizeof BTST2)) return false;
    if (!cmd_data(0x08, BTST3, sizeof BTST3)) return false;
    if (!cmd_data(0x30, PLL,   sizeof PLL))   return false;
    if (!cmd_data(0x50, CDI,   sizeof CDI))   return false;
    if (!cmd_data(0x60, TCON,  sizeof TCON))  return false;
    if (!cmd_data(0x61, TRES,  sizeof TRES))  return false;
    if (!cmd_data(0x84, TVDCS, sizeof TVDCS)) return false;
    if (!cmd_data(0xe3, PWS,   sizeof PWS))   return false;
    return wait_busy("init");
}

static void s6s_init(void)
{
    if (!run_init_sequence()) ESP_LOGE(TAG, "init sequence failed");
    else ESP_LOGI(TAG, "init complete");
}

/* PON -> DRF -> POF, framing an already-sent frame. Same tail as the
 * reference's spectra6_update() after the data write. */
static bool trigger_refresh(void)
{
    if (!cmd(0x04)) return false;                 /* power on */
    if (!wait_busy("power on")) return false;

    static const uint8_t DRF_V[] = {0x00};
    if (!cmd_data(0x12, DRF_V, sizeof DRF_V)) return false;   /* display refresh */
    if (!wait_busy("refresh")) return false;

    static const uint8_t POF_V[] = {0x00};
    if (!cmd_data(0x02, POF_V, sizeof POF_V)) return false;   /* power off */
    bool ok = wait_busy("power off");
    ESP_LOGI(TAG, "refresh done");
    return ok;
}

static void s6s_display(const uint8_t *image)
{
    if (!cmd(0x10)) return;                        /* data transfer (DTM) */
#ifdef EPD_ROTATE_180
    if (!send_buffer_rot180(image, EPD_BUF_BYTES)) return;
#else
    if (!send_buffer(image, EPD_BUF_BYTES)) return;
#endif
    if (!wait_busy("data")) return;
    trigger_refresh();
}

static void s6s_clear(uint8_t color)
{
    uint8_t packed = (color << 4) | color;
    uint8_t row[EPD_WIDTH / 2];                    /* 400 bytes */
    memset(row, packed, sizeof row);

    if (!cmd(0x10)) return;
    for (int y = 0; y < EPD_HEIGHT; y++)
        if (!send_buffer(row, sizeof row)) return;
    if (!wait_busy("data")) return;
    trigger_refresh();
}

/* Diagnostic: six palette bands top-to-bottom (see the base driver). */
static void s6s_show_color_bars(void)
{
    static const uint8_t palette[6] = {
        EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_YELLOW,
        EPD_COL_RED,   EPD_COL_BLUE,   EPD_COL_GREEN,
    };
    uint8_t row[EPD_WIDTH / 2];                    /* 400 bytes/row */

    if (!cmd(0x10)) return;
    for (int b = 0; b < 6; b++) {
        uint8_t packed = (palette[b] << 4) | palette[b];
        memset(row, packed, sizeof row);
        int band_h = EPD_HEIGHT / 6;
        if (b == 5) band_h += EPD_HEIGHT % 6;
        for (int y = 0; y < band_h; y++)
            if (!send_buffer(row, sizeof row)) return;
    }
    if (!wait_busy("data")) return;
    trigger_refresh();
}

static void s6s_show_palette_sweep(void)
{
    uint8_t row[EPD_WIDTH / 2];
    const int band_h = EPD_HEIGHT / 8;             /* 60 rows/band */

    if (!cmd(0x10)) return;
    for (uint8_t n = 0; n < 8; n++) {
        uint8_t packed = (n << 4) | n;
        memset(row, packed, sizeof row);
        for (int y = 0; y < band_h; y++)
            if (!send_buffer(row, sizeof row)) return;
    }
    if (!wait_busy("data")) return;
    trigger_refresh();
}

static void s6s_sleep(void)
{
    static const uint8_t DS_V[] = {0xa5};
    cmd_data(0x07, DS_V, sizeof DS_V);             /* deep sleep */
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);                /* EN low: cut panel power */
#endif
}

/* ---------- exported vtable ---------- */

const epd_driver_t spectra6_spi_single_driver = {
    .info = {
        .name      = "Spectra-6 single (800x480)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = s6s_port_init,
    .init               = s6s_init,
    .clear              = s6s_clear,
    .display            = s6s_display,
    .show_color_bars    = s6s_show_color_bars,
    .show_palette_sweep = s6s_show_palette_sweep,
    .sleep              = s6s_sleep,
};

#endif /* PANEL_DRIVER_SPECTRA6_SPI_SINGLE */
