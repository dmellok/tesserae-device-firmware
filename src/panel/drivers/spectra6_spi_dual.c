/*
 * Waveshare 13.3" Spectra E6 (6-color) e-paper driver -- Family A
 * (Spectra-6 SPI, dual-controller, 1200x1600).
 *
 * Ported from waveshareteam/ESP32-S3-ePaper-13.3E6 (epaper_port.c). The
 * init byte sequence and command set are panel-specific and must stay
 * byte-for-byte exact -- don't "clean them up."
 *
 * This is the original monolithic epd_driver.c body, unchanged in behavior:
 * the six public entry points are now file-static and exported through the
 * spectra6_spi_dual_driver vtable at the bottom. The emitted SPI/GPIO
 * sequence is identical to the pre-refactor driver.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_SPECTRA6_SPI_DUAL)

#include "drivers/spectra6_spi_dual.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* --- panel command opcodes (from datasheet via Waveshare demo) --- */
#define PSR             0x00
#define PWR             0x01
#define POF             0x02
#define PON             0x04
#define BTST_N          0x05
#define BTST_P          0x06
#define DTM             0x10  /* data transfer (frame data) */
#define DRF             0x12  /* display refresh */
#define CDI             0x50
#define TCON            0x60
#define TRES            0x61
#define AN_TM           0x74
#define AGID            0x86
#define BUCK_BOOST_VDDN 0xB0
#define TFT_VCOM_POWER  0xB1
#define EN_BUF          0xB6
#define BOOST_VDDP_EN   0xB7
#define CCSET           0xE0
#define PWS             0xE3
#define CMD66           0xF0
#define DEEP_SLEEP      0x07

/* --- canned init parameter blobs (do NOT edit) --- */
static const uint8_t PSR_V[]            = {0xDF, 0x69};
static const uint8_t PWR_V[]            = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t POF_V[]            = {0x00};
static const uint8_t DRF_V[]            = {0x00};
static const uint8_t CDI_V[]            = {0xF7};
static const uint8_t TCON_V[]           = {0x03, 0x03};
static const uint8_t TRES_V[]           = {0x04, 0xB0, 0x03, 0x20};
static const uint8_t CMD66_V[]          = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t EN_BUF_V[]         = {0x07};
static const uint8_t CCSET_V[]          = {0x01};
static const uint8_t PWS_V[]            = {0x22};
static const uint8_t AN_TM_V[]          = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
static const uint8_t AGID_V[]           = {0x10};
static const uint8_t BTST_P_V[]         = {0xE8, 0x28};
static const uint8_t BOOST_VDDP_EN_V[]  = {0x01};
static const uint8_t BTST_N_V[]         = {0xE8, 0x28};
static const uint8_t BUCK_BOOST_VDDN_V[]= {0x01};
static const uint8_t TFT_VCOM_POWER_V[] = {0x02};

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* ---------- low-level pin/SPI wrappers ---------- */

static inline void cs_both(int level)
{
    gpio_set_level(EPD_PIN_CS_M, level);
    gpio_set_level(EPD_PIN_CS_S, level);
}

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    /* Chunk large frames so we never blow past the bus DMA limit; 4 KiB
     * is comfortably below max_transfer_sz and keeps each transaction
     * short enough that other tasks aren't starved. */
    const size_t CHUNK = 4096;
    spi_transaction_t t;
    for (size_t off = 0; off < len; off += CHUNK) {
        size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
        memset(&t, 0, sizeof(t));
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

static void send_data_buf(const uint8_t *buf, size_t len)
{
    gpio_set_level(EPD_PIN_DC, 1);
    spi_tx_raw(buf, len);
}

static void send_data_byte(uint8_t b)
{
    send_data_buf(&b, 1);
}

static void cmd_with_data(uint8_t cmd, const uint8_t *buf, size_t len)
{
    send_cmd(cmd);
    send_data_buf(buf, len);
}

/* Block until BUSY goes HIGH (idle). Panel pulls it LOW while busy.
 * A full Spectra 6 refresh takes ~25-30s, so we only warn after 60s --
 * anything beyond that suggests an actually-stuck panel rather than
 * normal refresh time. */
static void wait_idle(void)
{
    int ticks = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!warned && ++ticks >= 6000) {
            ESP_LOGW(TAG, "BUSY still low after 60 s -- panel may be stuck");
            warned = true;
        }
    }
}

/* 5-pulse reset sequence (datasheet wants this many edges to latch). */
static void hw_reset(void)
{
    for (int i = 0; i < 3; i++) {
        gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
        gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(30));
    }
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
}

/* ---------- driver entry points (exported via the vtable below) ---------- */

static esp_err_t s6d_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask =
            (1ULL << EPD_PIN_RST)  | (1ULL << EPD_PIN_DC)  |
            (1ULL << EPD_PIN_CS_M) | (1ULL << EPD_PIN_CS_S) |
            (1ULL << EPD_PIN_PWR),
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

    /* Start with panel powered off and both CS lines high (idle). */
    gpio_set_level(EPD_PIN_RST,  1);
    gpio_set_level(EPD_PIN_CS_M, 1);
    gpio_set_level(EPD_PIN_CS_S, 1);
    gpio_set_level(EPD_PIN_PWR,  0);

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
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

/* The init sequence in the demo wraps every command in CS_M_0..CS_ALL(1);
 * a couple of commands intentionally only assert CS_M (the master controller
 * holds the shared registers). We preserve that distinction exactly. */
static void s6d_init(void)
{
    gpio_set_level(EPD_PIN_PWR, 1);   /* power-enable rail */
    vTaskDelay(pdMS_TO_TICKS(10));

    hw_reset();
    wait_idle();

    /* Master-only commands (chip-shared regs) */
    gpio_set_level(EPD_PIN_CS_M, 0);
    cmd_with_data(AN_TM, AN_TM_V, sizeof(AN_TM_V));
    cs_both(1);

    /* Broadcast commands to both controllers */
    #define BOTH(cmd, val) do { \
        cs_both(0); \
        cmd_with_data((cmd), (val), sizeof(val)); \
        cs_both(1); \
    } while (0)

    BOTH(CMD66, CMD66_V);
    BOTH(PSR,   PSR_V);
    BOTH(CDI,   CDI_V);
    BOTH(TCON,  TCON_V);
    BOTH(AGID,  AGID_V);
    BOTH(PWS,   PWS_V);
    BOTH(CCSET, CCSET_V);
    BOTH(TRES,  TRES_V);

    #undef BOTH

    /* Final block: master-only power/boost programming */
    #define MASTER(cmd, val) do { \
        gpio_set_level(EPD_PIN_CS_M, 0); \
        cmd_with_data((cmd), (val), sizeof(val)); \
        cs_both(1); \
    } while (0)

    MASTER(PWR,             PWR_V);
    MASTER(EN_BUF,          EN_BUF_V);
    MASTER(BTST_P,          BTST_P_V);
    MASTER(BOOST_VDDP_EN,   BOOST_VDDP_EN_V);
    MASTER(BTST_N,          BTST_N_V);
    MASTER(BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V);
    MASTER(TFT_VCOM_POWER,  TFT_VCOM_POWER_V);

    #undef MASTER

    ESP_LOGI(TAG, "init complete");
}

/* PON -> wait -> DRF -> wait -> POF. This is the "actually paint pixels" step. */
static void trigger_refresh(void)
{
    cs_both(0); send_cmd(PON); cs_both(1);
    wait_idle();

    vTaskDelay(pdMS_TO_TICKS(50));
    cs_both(0); cmd_with_data(DRF, DRF_V, sizeof(DRF_V)); cs_both(1);
    wait_idle();

    vTaskDelay(pdMS_TO_TICKS(50));
    cs_both(0); cmd_with_data(POF, POF_V, sizeof(POF_V)); cs_both(1);
    ESP_LOGI(TAG, "refresh done");
}

static void s6d_clear(uint8_t color)
{
    /* Each half is 300 bytes wide x 1600 rows = 480000 bytes. We allocate
     * one half-buffer in PSRAM and reuse it for both controllers. */
    const size_t HALF = EPD_BUF_BYTES / 2;
    uint8_t *buf = heap_caps_malloc(HALF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte clear buffer", (unsigned)HALF);
        return;
    }
    memset(buf, (color << 4) | color, HALF);

    gpio_set_level(EPD_PIN_CS_M, 0);
    send_cmd(DTM);
    send_data_buf(buf, HALF);
    cs_both(1);

    gpio_set_level(EPD_PIN_CS_S, 0);
    send_cmd(DTM);
    send_data_buf(buf, HALF);
    cs_both(1);

    free(buf);
    trigger_refresh();
}

static void s6d_display(const uint8_t *image)
{
    /* Frame layout (packed 4bpp, 2 px/byte):
     *   row stride = 600 bytes; left half = bytes [0..299], right = [300..599]
     * The panel wants each half streamed contiguously, so we de-interleave
     * the input into a scratch half-buffer rather than DMA'ing 1600 tiny
     * 300-byte transactions (which would 10x the SPI overhead).            */
    const size_t HALF_ROW = EPD_WIDTH / 4;     /* 300 */
    const size_t FULL_ROW = HALF_ROW * 2;      /* 600 */
    const size_t HALF_BUF = HALF_ROW * EPD_HEIGHT;  /* 480000 */

    uint8_t *scratch = heap_caps_malloc(HALF_BUF, MALLOC_CAP_SPIRAM);
    if (!scratch) {
        ESP_LOGE(TAG, "OOM allocating %u-byte display scratch", (unsigned)HALF_BUF);
        return;
    }

    /* Left half */
    for (size_t r = 0; r < EPD_HEIGHT; r++) {
        memcpy(scratch + r * HALF_ROW,
               image  + r * FULL_ROW,
               HALF_ROW);
    }
    gpio_set_level(EPD_PIN_CS_M, 0);
    send_cmd(DTM);
    send_data_buf(scratch, HALF_BUF);
    cs_both(1);

    /* Right half */
    for (size_t r = 0; r < EPD_HEIGHT; r++) {
        memcpy(scratch + r * HALF_ROW,
               image  + r * FULL_ROW + HALF_ROW,
               HALF_ROW);
    }
    gpio_set_level(EPD_PIN_CS_S, 0);
    send_cmd(DTM);
    send_data_buf(scratch, HALF_BUF);
    cs_both(1);

    free(scratch);
    trigger_refresh();
}

static void s6d_show_color_bars(void)
{
    /* Six horizontal bands, full panel width. Each band is HEIGHT/6 = 266
     * rows tall, with the final band absorbing the 4-row remainder. We
     * stream one half-row (300 bytes) per band repeated N times, rather
     * than allocating a full 480 KB scratch buffer for a constant pattern. */
    static const uint8_t palette[6] = {
        EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_YELLOW,
        EPD_COL_RED,   EPD_COL_BLUE,   EPD_COL_GREEN,
    };
    const size_t HALF_ROW = EPD_WIDTH / 4;     /* 300 */
    uint8_t row[HALF_ROW];

    for (int side = 0; side < 2; side++) {
        gpio_set_level(side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S, 0);
        send_cmd(DTM);
        for (int b = 0; b < 6; b++) {
            uint8_t packed = (palette[b] << 4) | palette[b];
            memset(row, packed, HALF_ROW);

            size_t band_h = EPD_HEIGHT / 6;
            if (b == 5) band_h += EPD_HEIGHT % 6;
            for (size_t r = 0; r < band_h; r++) {
                send_data_buf(row, HALF_ROW);
            }
        }
        cs_both(1);
    }
    trigger_refresh();
}

static void s6d_show_palette_sweep(void)
{
    /* 8 horizontal bands of HEIGHT/8 = 200 rows each, one per possible
     * nibble value 0x0..0x7. Stream a 300-byte half-row N times per band
     * rather than allocating a 480 KB scratch -- this pattern compresses
     * trivially to a memset. */
    const size_t HALF_ROW = EPD_WIDTH / 4;     /* 300 */
    const size_t BAND_H   = EPD_HEIGHT / 8;    /* 200 */
    uint8_t row[HALF_ROW];

    for (int side = 0; side < 2; side++) {
        /* side 0 = left controller (CS_M), side 1 = right (CS_S) */
        gpio_set_level(side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S, 0);
        send_cmd(DTM);
        for (uint8_t n = 0; n < 8; n++) {
            uint8_t packed = (n << 4) | n;
            memset(row, packed, HALF_ROW);
            for (size_t r = 0; r < BAND_H; r++) {
                send_data_buf(row, HALF_ROW);
            }
        }
        cs_both(1);
    }
    trigger_refresh();
}

static void s6d_sleep(void)
{
    cs_both(0);
    send_cmd(DEEP_SLEEP);
    send_data_byte(0xA5);             /* magic deep-sleep arg per datasheet */
    cs_both(1);

    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(EPD_PIN_PWR, 0);
    gpio_set_level(EPD_PIN_RST, 0);
}

/* ---------- exported vtable ---------- */

const epd_driver_t spectra6_spi_dual_driver = {
    .info = {
        .name      = "Spectra-6 13.3\" dual (1200x1600)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = s6d_port_init,
    .init               = s6d_init,
    .clear              = s6d_clear,
    .display            = s6d_display,
    .show_color_bars    = s6d_show_color_bars,
    .show_palette_sweep = s6d_show_palette_sweep,
    .sleep              = s6d_sleep,
};

#endif /* PANEL_DRIVER_SPECTRA6_SPI_DUAL */
