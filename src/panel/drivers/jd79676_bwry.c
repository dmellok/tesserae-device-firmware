/*
 * Seeed 2.13" quadruple-colour BWRY -- JD79676 controller, 122x250 visible.
 *
 * Single-controller SPI panel with a DC command/data line, the first 2bpp
 * indexed-colour driver in this repo (mono_spi's BWR path is two 1bpp
 * planes; this controller takes ONE interleaved 2bpp buffer). Init, refresh
 * and sleep sequences are transcribed from Seeed_GFX
 * (TFT_Drivers/JD79676_Defines.h: EPD_INIT / EPD_UPDATE / EPD_SLEEP), which
 * drives this glass in the stock EE05 examples. Do not "clean up" the init
 * bytes -- they are the vendor sequence, byte for byte.
 *
 * Geometry: the controller scans 128 columns but only 122 reach the glass
 * (Seeed_GFX COL_OFFSET 6). The server packs at the full 128-column native
 * stride and pads the hidden columns, so this driver streams EPD_BUF_BYTES
 * = 128*250*2/8 = 8000 bytes verbatim: no windowing, no repacking. Which
 * edge the hidden columns sit on is a server-side fact (the manifest's
 * col_offset); the selftest pattern makes it readable off a photo.
 *
 * Wire format in: 2bpp packed, 4 px/byte, MSB pair = leftmost pixel, using
 * the server's bwry_4 palette indices 0=black 1=white 2=yellow 3=red.
 * The JD79676 wants its own 2-bit codes (from Seeed_GFX COLOR_GET):
 * 0=white 1=black 2=yellow 3=red -- black and white swapped, inks in
 * place. display() translates per byte through a 256-entry LUT.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_JD79676_BWRY)

#include "drivers/jd79676_bwry.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_jd79676";

/* JD79676 opcodes (UC81xx-style command set) */
#define PSR   0x00
#define PWR   0x01
#define POF   0x02
#define PON   0x04
#define DSLP  0x07
#define DTM1  0x10   /* data start transmission (the 2bpp frame) */
#define DRF   0x12   /* display refresh */
#define CDI   0x50
#define TCON  0x60
#define TRES  0x61

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* Server bwry_4 index -> JD79676 wire code, per 2-bit field.
 * 0 (black) -> 1, 1 (white) -> 0, 2 (yellow) -> 2, 3 (red) -> 3. */
static inline uint8_t wire_code(uint8_t idx)
{
    return (idx < 2) ? (uint8_t)(idx ^ 1) : idx;
}

/* Whole-byte translation (four 2-bit fields at once), built in port_init. */
static uint8_t s_byte_lut[256];

/* ---------- low-level SPI/GPIO (mono_spi transport, single CS) ---------- */

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
            ESP_LOGE(TAG, "spi tx fail at off=%u: %s", (unsigned)off, esp_err_to_name(err));
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

/* One CS-framed command with optional trailing data. */
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(cmd);
    if (len) send_data(data, len);
    gpio_set_level(EPD_PIN_CS, 1);
}

/* BUSY handling mirrors mono_spi: active-LOW busy, bounded waits, and the
 * assert-before-idle handshake that stops a refresh being cut short when
 * wait_idle() samples before the controller has pulled BUSY down. A small
 * panel refreshes in ~15-25 s worst case; the 45 s ceiling is inherited
 * because timing out and powering down is the safe failure either way. */
#define BUSY_IDLE_STABLE_SAMPLES 3
#define BUSY_ASSERT_TIMEOUT_MS 1000
#define BUSY_WAIT_TIMEOUT_MS 45000

static bool wait_idle(void)
{
    int waited = 0, high = 0;
    bool warned = false;
    while (high < BUSY_IDLE_STABLE_SAMPLES) {
        high = (gpio_get_level(EPD_PIN_BUSY) == 0) ? 0 : high + 1;
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        if (!warned && waited >= 10000) {
            ESP_LOGW(TAG, "BUSY still low after 10 s; panel may be slow or stuck");
            warned = true;
        }
        if (waited >= BUSY_WAIT_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY never cleared in %d ms; powering the panel down "
                          "anyway rather than leaving it driven",
                     BUSY_WAIT_TIMEOUT_MS);
            return false;
        }
    }
    return true;
}

static bool wait_busy_asserted(void)
{
    for (int waited = 0; waited < BUSY_ASSERT_TIMEOUT_MS; waited += 2) {
        if (gpio_get_level(EPD_PIN_BUSY) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

/* Seeed's EPD_WAKEUP reset timing: 20 ms low, 20 ms high, then BUSY. */
static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------- driver entry points ---------- */

static esp_err_t jd_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    for (int b = 0; b < 256; b++) {
        uint8_t o = 0;
        for (int p = 0; p < 4; p++) {
            uint8_t idx = (uint8_t)((b >> (6 - 2 * p)) & 0x3);
            o = (uint8_t)((o << 2) | wire_code(idx));
        }
        s_byte_lut[b] = o;
    }

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_RST) | (1ULL << EPD_PIN_DC) | (1ULL << EPD_PIN_CS)
#ifdef EPD_PIN_PWR
                      | (1ULL << EPD_PIN_PWR)
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
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);
#endif

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
    esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(bus_err);
    }
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

/* Seeed_GFX EPD_INIT, in order, ending in PON + BUSY wait. Several of the
 * registers past TRES (0xE7/0xE3/0xB4/0xB5/0xE9) have no public datasheet
 * name; they stay exactly as the vendor sends them. */
static const uint8_t V_4D[]   = {0x78};
static const uint8_t PSR_V[]  = {0x0f, 0x29};
static const uint8_t PWR_V[]  = {0x07};
static const uint8_t V_03[]   = {0x10, 0x54, 0x44};
static const uint8_t BTST_V[] = {0x0f, 0x0a, 0x2f, 0x25, 0x22, 0x2e, 0x21};
static const uint8_t CDI_V[]  = {0x37};
static const uint8_t TCON_V[] = {0x02, 0x02};
static const uint8_t TRES_V[] = {
    EPD_WIDTH >> 8, EPD_WIDTH & 0xff,      /* 0x0080 = 128 */
    EPD_HEIGHT >> 8, EPD_HEIGHT & 0xff,    /* 0x00fa = 250 */
};
static const uint8_t V_E7[]   = {0x1c};
static const uint8_t V_E3[]   = {0x22};
static const uint8_t V_B4[]   = {0xd0};
static const uint8_t V_B5[]   = {0x03};
static const uint8_t V_E9[]   = {0x01};
static const uint8_t PLL_V[]  = {0x08};

static void jd_init(void)
{
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 1);   /* EN: enable panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    hw_reset();
    wait_idle();

    cmd_data(0x4d, V_4D,   sizeof V_4D);
    cmd_data(PSR,  PSR_V,  sizeof PSR_V);
    cmd_data(PWR,  PWR_V,  sizeof PWR_V);
    cmd_data(0x03, V_03,   sizeof V_03);
    cmd_data(0x06, BTST_V, sizeof BTST_V);
    cmd_data(CDI,  CDI_V,  sizeof CDI_V);
    cmd_data(TCON, TCON_V, sizeof TCON_V);
    cmd_data(TRES, TRES_V, sizeof TRES_V);
    cmd_data(0xe7, V_E7,   sizeof V_E7);
    cmd_data(0xe3, V_E3,   sizeof V_E3);
    cmd_data(0xb4, V_B4,   sizeof V_B4);
    cmd_data(0xb5, V_B5,   sizeof V_B5);
    cmd_data(0xe9, V_E9,   sizeof V_E9);
    cmd_data(0x30, PLL_V,  sizeof PLL_V);
    cmd_data(PON,  NULL, 0);
    wait_idle();

    ESP_LOGI(TAG, "init complete (JD79676 BWRY)");
}

/* Seeed's EPD_UPDATE: DRF carries one 0x00 parameter byte. POF is NOT sent
 * here -- it belongs to the sleep sequence (EPD_SLEEP), matching the vendor
 * lifecycle: init -> data -> refresh [-> refresh...] -> sleep. */
static void trigger_refresh(void)
{
    static const uint8_t DRF_V[] = {0x00};
    cmd_data(DRF, DRF_V, sizeof DRF_V);
    int64_t t0 = esp_timer_get_time();
    if (!wait_busy_asserted())
        ESP_LOGW(TAG, "panel never asserted BUSY %d ms after DRF; "
                      "refresh may not have started",
                 BUSY_ASSERT_TIMEOUT_MS);
    wait_idle();
    ESP_LOGI(TAG, "refresh done (%lld ms)",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void jd_display(const uint8_t *image)
{
    uint8_t row[EPD_WIDTH / 4];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM1);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        const uint8_t *in = image + y * (EPD_WIDTH / 4);
        for (int b = 0; b < EPD_WIDTH / 4; b++) row[b] = s_byte_lut[in[b]];
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}

/* Emit `rows` rows of a single palette index (caller sent DTM1, CS low). */
static void send_solid_rows(uint8_t idx, int rows)
{
    uint8_t code = wire_code((uint8_t)(idx & 0x3));
    uint8_t fill = (uint8_t)(code << 6 | code << 4 | code << 2 | code);
    uint8_t row[EPD_WIDTH / 4];
    memset(row, fill, sizeof row);
    for (int y = 0; y < rows; y++) send_data(row, sizeof row);
}

static void jd_clear(uint8_t color)
{
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM1);
    send_solid_rows(color, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}

/* Selftest pattern: the four inks as horizontal bands PLUS asymmetric
 * column markers that make the hidden-column split readable off a photo.
 * The controller drives 128 columns but the glass shows only 122; which
 * six disappear decides the server manifest's col_offset.
 *
 *   cols 0-5    BLACK   candidate hidden zone A
 *   cols 6-9    YELLOW  left-edge marker
 *   cols 118-121 RED    right-edge marker
 *   cols 122-127 BLACK  candidate hidden zone B
 *   in between: black / white / yellow / red bands, top to bottom
 *
 * Reading the photo: a YELLOW stripe flush with one edge means the black
 * zone on that side is hidden (offset 6 if it's the first-column side);
 * black columns still visible at an edge count the split directly. Yellow
 * left vs red right also exposes a mirrored scan, and the band order
 * exposes an inverted row direction. */
static void jd_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 4;
    uint8_t row[EPD_WIDTH / 4];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM1);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        uint8_t band = (uint8_t)(y / BAND_H);
        if (band > 3) band = 3;                  /* 250 % 4 remainder rows */
        for (int x = 0; x < EPD_WIDTH; x++) {
            uint8_t idx;
            if (x <= 5)        idx = 0;          /* black: hidden zone A   */
            else if (x <= 9)   idx = 2;          /* yellow: left marker    */
            else if (x >= 122) idx = 0;          /* black: hidden zone B   */
            else if (x >= 118) idx = 3;          /* red: right marker      */
            else               idx = band;
            uint8_t code = wire_code(idx);
            int shift = 6 - 2 * (x & 3);
            if ((x & 3) == 0) row[x >> 2] = 0;
            row[x >> 2] |= (uint8_t)(code << shift);
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}

/* Diagnostic: the four RAW wire codes (0..3, no translation) as bands.
 * If jd_show_color_bars renders unexpected inks, this reads the panel's
 * true code->ink map so the wire_code table can be corrected. */
static void jd_show_palette_sweep(void)
{
    const int BAND_H = EPD_HEIGHT / 4;
    uint8_t row[EPD_WIDTH / 4];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM1);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        uint8_t code = (uint8_t)(y / BAND_H);
        if (code > 3) code = 3;
        uint8_t fill = (uint8_t)(code << 6 | code << 4 | code << 2 | code);
        memset(row, fill, sizeof row);
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}

/* Seeed's EPD_SLEEP: POF + BUSY, settle, then DSLP with the 0xA5 magic.
 * EN is dropped last so the panel is never left driven on a dead rail. */
static void jd_sleep(void)
{
    cmd_data(POF, NULL, 0);
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t magic = 0xA5;
    cmd_data(DSLP, &magic, 1);
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);       /* EN low: cut panel power */
#endif
}

/* ---------- exported vtable ---------- */

const epd_driver_t jd79676_bwry_driver = {
    .info = {
        .name      = "BWRY 2.13\" (JD79676, 128x250 stride, 2bpp)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 2,
        .buf_bytes = EPD_BUF_BYTES,
        .grayscale = false,    /* four fixed inks: a palette, not gray levels */
    },
    .port_init          = jd_port_init,
    .init               = jd_init,
    .clear              = jd_clear,
    .display            = jd_display,
    .show_color_bars    = jd_show_color_bars,
    .show_palette_sweep = jd_show_palette_sweep,
    .sleep              = jd_sleep,
};

#endif /* PANEL_DRIVER_JD79676_BWRY */
