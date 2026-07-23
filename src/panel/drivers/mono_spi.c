/*
 * Seeed reTerminal E1001 -- 7.5" mono (black/white) 800x480, Family C.
 *
 * UC8179-class single-controller panel driven over SPI with a DC command/data
 * line. The init and refresh sequences are ported from bb_epaper's
 * EP75_800x480 panel (epd75_init_sequence_full; the panel for
 * BOARD_SEEED_RETERMINAL_E1001): PWR / PON / PSR=0x1F / TRES / dual-SPI-off /
 * CDI / TCON, then the 1bpp frame to DTM2 (0x13) and a DRF (0x12) refresh.
 * Transport mirrors the 13.3E6 base driver (DC pin, single CS, BUSY-low-busy),
 * just single-controller and monochrome.
 *
 * The frame is a packed 1bpp bitmap, EPD_BUF_BYTES = W*H/8 = 48000 bytes,
 * bit 1 = white. Every wake re-runs init() (full reset + init) before
 * display(), which is the "re-init before refresh" behaviour these
 * UC8179-class panels need, so no separate workaround is required.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_MONO_SPI)

#include "drivers/mono_spi.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_mono";

/* UC8179 opcodes */
#define PSR   0x00
#define PWR   0x01
#define POF   0x02
#define PON   0x04
#define DSLP  0x07
#define DTM2  0x13   /* data start transmission 2 (the B/W image) */
#define DRF   0x12   /* display refresh */
#define CDI   0x50
#define TCON  0x60
#define TRES  0x61

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

/* Block while BUSY is low (busy). UC81xx idle == BUSY high. ~60 s cap. */
static void wait_idle(void)
{
    int ticks = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (!warned && ++ticks >= 6000) {
            ESP_LOGW(TAG, "BUSY low after 60 s -- panel may be stuck");
            warned = true;
        }
    }
}

static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

/* ---------- driver entry points ---------- */

static esp_err_t mono_port_init(void)
{
    if (s_port_inited) return ESP_OK;

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
    /* The SD card may share this bus (reTerminal boards) and have initialised
     * it already -- with a MISO line and a full-frame transfer cap, so
     * inheriting it is safe. Tolerate INVALID_STATE like spectra6_spi_single. */
    esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(bus_err);
    }
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

/* Init parameter blobs (bb_epaper epd75_init_sequence_full; do NOT edit). */
static const uint8_t PWR_V[]  = {0x07, 0x07, 0x3f, 0x3f};
static const uint8_t PSR_V[]  = {0x1f};
static const uint8_t TRES_V[] = {0x03, 0x20, 0x01, 0xe0};   /* 0x0320=800 x 0x01e0=480 */
static const uint8_t DSPI_V[] = {0x00};                     /* cmd 0x15: dual-SPI off */
static const uint8_t CDI_V[]  = {0x21, 0x07};
static const uint8_t TCON_V[] = {0x22};

#ifdef EPD_GRAY4
/* ------------------------------------------------------------------ */
/* 4-level grayscale (EPD_GRAY4 builds; kind seeed_reterminal_e1001_gray)
 *
 * The stock OTP waveform only knows black/white, so 4-gray drives the panel
 * from REGISTER LUTs (PSR 0xBF selects them) and sends BOTH image planes --
 * DTM1 (0x10, "old") and DTM2 (0x13, "new") -- so each pixel carries 2 bits
 * that land on four optical states in one refresh:
 *
 *        white  light-gray  dark-gray  black          (g = 3    2    1    0)
 *   0x10:  1        1           0        0            (bit = (g >> 1) & 1)
 *   0x13:  1        0           1        0            (bit =  g       & 1)
 *
 * Wire format in: 2bpp packed, 4 px/byte, MSB-first (bits 7-6 = leftmost),
 * 0b00 = black .. 0b11 = white, 96000 bytes. LUTs + init values are the
 * GoodDisplay official GDEY075T7 4-gray demo (42 bytes each), corroborated
 * byte-for-byte by GxEPD2_4G (GxEPD2_750_T7) -- the panel class of the OG
 * TRMNL and the reTerminal E1001. Register LUTs are NOT temperature
 * compensated: expect a bench tuning pass (and see bb_epaper's variants if
 * this batch renders too light/dark). 4-gray is always a full refresh. */
static const uint8_t LUT_VCOM_4G[42] = {
    0x00, 0x0A, 0x00, 0x00, 0x00, 0x01,
    0x60, 0x14, 0x14, 0x00, 0x00, 0x01,
    0x00, 0x14, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x13, 0x0A, 0x01, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WW_4G[42] = {   /* R21, also the border LUT (R25) */
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01,
    0x90, 0x14, 0x14, 0x00, 0x00, 0x01,
    0x10, 0x14, 0x0A, 0x00, 0x00, 0x01,
    0xA0, 0x13, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BW_4G[42] = {   /* R22 */
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01,
    0x90, 0x14, 0x14, 0x00, 0x00, 0x01,
    0x00, 0x14, 0x0A, 0x00, 0x00, 0x01,
    0x99, 0x0C, 0x01, 0x03, 0x04, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WB_4G[42] = {   /* R23 */
    0x40, 0x0A, 0x00, 0x00, 0x00, 0x01,
    0x90, 0x14, 0x14, 0x00, 0x00, 0x01,
    0x00, 0x14, 0x0A, 0x00, 0x00, 0x01,
    0x99, 0x0B, 0x04, 0x04, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BB_4G[42] = {   /* R24 */
    0x80, 0x0A, 0x00, 0x00, 0x00, 0x01,
    0x90, 0x14, 0x14, 0x00, 0x00, 0x01,
    0x20, 0x14, 0x0A, 0x00, 0x00, 0x01,
    0x50, 0x13, 0x01, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t BTST_4G[] = {0x17, 0x17, 0x28, 0x17};
static const uint8_t PSR_4G[]  = {0xbf};        /* KW mode, LUTs from register */
static const uint8_t PLL_4G[]  = {0x06};        /* 50 Hz */
static const uint8_t VDCS_4G[] = {0x12};
static const uint8_t CDI_4G[]  = {0x10, 0x07};

/* Stream one 1bpp plane derived from the 2bpp frame: plane bit =
 * (g >> shift) & 1, 8 px/byte MSB-first, row by row. */
static void gray_send_plane(uint8_t dtm_cmd, const uint8_t *image, int shift)
{
    uint8_t row[EPD_WIDTH / 8];
    const uint8_t *in = image;

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(dtm_cmd);
    for (int y = 0; y < EPD_HEIGHT; y++) {
        for (int b = 0; b < EPD_WIDTH / 8; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 2; k++) {           /* 2 input bytes -> 8 px */
                uint8_t v = *in++;
                for (int p = 0; p < 4; p++) {
                    uint8_t g = (uint8_t)((v >> (6 - 2 * p)) & 0x3);
                    o = (uint8_t)((o << 1) | ((g >> shift) & 1));
                }
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Emit `rows` solid plane rows (caller has sent the DTM command, CS low). */
static void gray_send_solid_rows(uint8_t fill, int rows)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, fill, sizeof row);
    for (int y = 0; y < rows; y++) send_data(row, sizeof row);
}
#endif /* EPD_GRAY4 */

static void mono_init(void)
{
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 1);   /* EN: enable panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    hw_reset();

#ifdef EPD_GRAY4
    cmd_data(0x06, BTST_4G, sizeof BTST_4G);   /* booster soft start */
    cmd_data(PWR,  PWR_V,   sizeof PWR_V);
    cmd_data(PON,  NULL, 0);
    wait_idle();
    cmd_data(PSR,  PSR_4G,  sizeof PSR_4G);
    cmd_data(0x30, PLL_4G,  sizeof PLL_4G);
    cmd_data(TRES, TRES_V,  sizeof TRES_V);
    cmd_data(0x15, DSPI_V,  sizeof DSPI_V);
    cmd_data(TCON, TCON_V,  sizeof TCON_V);
    cmd_data(0x82, VDCS_4G, sizeof VDCS_4G);
    cmd_data(CDI,  CDI_4G,  sizeof CDI_4G);
    cmd_data(0x20, LUT_VCOM_4G, sizeof LUT_VCOM_4G);
    cmd_data(0x21, LUT_WW_4G,   sizeof LUT_WW_4G);
    cmd_data(0x22, LUT_BW_4G,   sizeof LUT_BW_4G);
    cmd_data(0x23, LUT_WB_4G,   sizeof LUT_WB_4G);
    cmd_data(0x24, LUT_BB_4G,   sizeof LUT_BB_4G);
    cmd_data(0x25, LUT_WW_4G,   sizeof LUT_WW_4G);   /* border */
    ESP_LOGI(TAG, "init complete (4-gray register LUTs)");
#else
    cmd_data(PWR, PWR_V, sizeof PWR_V);
    cmd_data(PON, NULL, 0);            /* power on */
    wait_idle();
    cmd_data(PSR,  PSR_V,  sizeof PSR_V);
    cmd_data(TRES, TRES_V, sizeof TRES_V);
    cmd_data(0x15, DSPI_V, sizeof DSPI_V);
    cmd_data(CDI,  CDI_V,  sizeof CDI_V);
    cmd_data(TCON, TCON_V, sizeof TCON_V);

    ESP_LOGI(TAG, "init complete");
#endif
}

/* Refresh an already-loaded frame, then power off. */
static void trigger_refresh(void)
{
    cmd_data(DRF, NULL, 0);
    wait_idle();
    cmd_data(POF, NULL, 0);
    wait_idle();
    ESP_LOGI(TAG, "refresh done");
}

static void mono_display(const uint8_t *image)
{
#ifdef EPD_GRAY4
    /* Both planes, derived on the fly from the 2bpp buffer (see table). */
    gray_send_plane(0x10, image, 1);   /* DTM1: bit = (g >> 1) & 1 */
    gray_send_plane(DTM2, image, 0);   /* DTM2: bit =  g       & 1 */
    trigger_refresh();
#else
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    send_data(image, EPD_BUF_BYTES);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
#endif
}

static void mono_clear(uint8_t color)
{
#ifdef EPD_GRAY4
    /* Solid gray level: both planes carry that level's bit pattern. */
    uint8_t g  = (uint8_t)(color & 0x3);
    uint8_t p1 = ((g >> 1) & 1) ? 0xFF : 0x00;
    uint8_t p2 = (g & 1)        ? 0xFF : 0x00;
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    gray_send_solid_rows(p1, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    gray_send_solid_rows(p2, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
#else
    /* color: EPD_COL_WHITE -> all-white (0xFF), else all-black (0x00). */
    uint8_t fill = (color == EPD_COL_WHITE) ? 0xFF : 0x00;
    uint8_t row[EPD_WIDTH / 8];
    memset(row, fill, sizeof row);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int y = 0; y < EPD_HEIGHT; y++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
#endif
}

#ifdef EPD_GRAY4
/* Gray-ramp selftest: 4 horizontal bands, black -> dark gray -> light gray ->
 * white, top to bottom. Even spacing between the two grays is the bench
 * tuning target (LUT variants exist if a batch renders too light/dark). */
static void mono_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 4;   /* 120 rows/band */

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows(((g >> 1) & 1) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows((g & 1) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}
#else
/* Diagnostic: 8 alternating black/white horizontal bands. On a healthy panel
 * this shows crisp stripes; smearing or a blank screen points at the init or
 * the data transport. */
static void mono_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 8;   /* 60 rows/band */
    uint8_t row[EPD_WIDTH / 8];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int b = 0; b < 8; b++) {
        memset(row, (b & 1) ? 0x00 : 0xFF, sizeof row);   /* white, black, ... */
        for (int y = 0; y < BAND_H; y++) send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}
#endif /* EPD_GRAY4 */

/* Diagnostic: vertical stripe pattern -- a finer transport check. In 4-gray
 * mode both planes get the pattern (alternating black/white pixels); sending
 * a single plane would pair fresh data with a stale second plane. */
static void mono_show_palette_sweep(void)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, 0xAA, sizeof row);       /* alternating pixels */

#ifdef EPD_GRAY4
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    for (int y = 0; y < EPD_HEIGHT; y++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);
#endif
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int y = 0; y < EPD_HEIGHT; y++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
}

static void mono_sleep(void)
{
    uint8_t magic = 0xA5;
    cmd_data(DSLP, &magic, 1);            /* deep sleep */
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);       /* EN low: cut panel power */
#endif
}

/* ---------- exported vtable ---------- */

const epd_driver_t mono_spi_driver = {
    .info = {
#ifdef EPD_GRAY4
        .name      = "Mono 7.5\" (800x480, 4-gray 2bpp)",
        .bpp       = 2,
#else
        .name      = "Mono 7.5\" (800x480, 1bpp)",
        .bpp       = 1,
#endif
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = mono_port_init,
    .init               = mono_init,
    .clear              = mono_clear,
    .display            = mono_display,
    .show_color_bars    = mono_show_color_bars,
    .show_palette_sweep = mono_show_palette_sweep,
    .sleep              = mono_sleep,
};

#endif /* PANEL_DRIVER_MONO_SPI */
