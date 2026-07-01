/*
 * Seeed reTerminal E1004 -- 13.3" T133A01 dual-chip Spectra-6 (1200x1600).
 *
 * Family A variant of the dual-controller Spectra-6 driver. The transport is
 * the same as the base 13.3E6 (spectra6_spi_dual.c): one shared SPI bus, two
 * hand-driven chip-selects, the frame split into a left half (primary/CS_M)
 * and a right half (second/CS_S), 4bpp packed Spectra-6 data. What differs is
 * panel silicon: the T133A01 has its own init sequence, a per-frame CCSET, and
 * refreshes with DRF=0x01 -- so it can NOT reuse the Waveshare init blobs.
 *
 * Every panel-specific value below is ported verbatim from the limengdu
 * bb_epaper fork pinned at commit 95fd94afe39cd7db32bef7c70eea06d654264ff6:
 *   - init:    src/arduino_io.inl  bbepT133A01InitIO()  + bbepT133A01WriteCommandData()
 *   - data:    src/bb_ep.inl       bbepWriteImage4bppDual() (T133A01 branch) + t133a01_write_half()
 *   - refresh: src/bb_ep.inl       bbepRefresh()  (T133A01 branch) via t133a01_update_phase()
 *   - sleep:   src/bb_ep.inl       bbepSleep()    (T133A01 branch)
 * The `bBoth` flag on each init command (CS1-only vs both controllers) is
 * preserved exactly; it happens to mirror the 13.3E6's master-only/broadcast
 * split, differing only in which registers are programmed.
 *
 * NOTE: UNVERIFIED ON HARDWARE. The sequence matches an unmerged Arduino PR;
 * confirm on a real E1004 before trusting a flash.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_SPECTRA6_T133A01_DUAL)

#include "drivers/spectra6_t133a01_dual.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_t133a01";

/* --- UC81xx command opcodes used by the T133A01 --- */
#define PSR         0x00
#define PWR         0x01
#define POF         0x02   /* power off */
#define DEEP_SLEEP  0x07
#define PON         0x04
#define BTST_N      0x05
#define BTST_P      0x06
#define DTM         0x10   /* data transfer (frame data) */
#define DRF         0x12   /* display refresh */
#define CDI         0x50
#define TCON        0x60
#define TRES        0x61
#define AN_TM       0x74
#define AGID        0x86
#define DCDC        0xA5   /* T133A01-specific DC/DC setting (not on 13.3E6) */
#define VDDN        0xB0
#define VCOM_PWR    0xB1
#define EN_BUF      0xB6
#define VDDP_EN     0xB7
#define CCSET       0xE0
#define PWS         0xE3
#define CMD66       0xF0

/* --- T133A01 init parameter blobs (bbepT133A01InitIO, do NOT edit) --- */
static const uint8_t AN_TM_V[]    = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
static const uint8_t CMD66_V[]    = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t PSR_V[]      = {0xDF, 0x69};
static const uint8_t DCDC_V[]     = {0x44, 0x54, 0x00};
static const uint8_t CDI_V[]      = {0x37};
static const uint8_t TCON_V[]     = {0x03, 0x03};
static const uint8_t AGID_V[]     = {0x10};
static const uint8_t PWS_V[]      = {0x22};
static const uint8_t TRES_V[]     = {0x04, 0xB0, 0x03, 0x20};
static const uint8_t PWR_V[]      = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t EN_BUF_V[]   = {0x07};
static const uint8_t BTST_P_V[]   = {0xE0, 0x20};
static const uint8_t VDDP_EN_V[]  = {0x01};
static const uint8_t BTST_N_V[]   = {0xE0, 0x20};
static const uint8_t VDDN_V[]     = {0x01};
static const uint8_t VCOM_PWR_V[] = {0x02};
static const uint8_t CCSET_V[]    = {0x01};   /* per-frame "current frame" */
static const uint8_t DRF_V[]      = {0x01};   /* T133A01 refreshes with DRF=0x01 */
static const uint8_t POF_V[]      = {0x00};

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* ---------- low-level pin/SPI wrappers ---------- */

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    /* Chunk large frames so we never blow past the bus DMA limit; matches the
     * 13.3E6 driver's 4 KiB chunking. */
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

/* Raw command+data write to an explicit set of chip-selects. Assert whichever
 * of CS_M (primary) / CS_S (second) is requested, low, for the duration of the
 * transfer. Init and per-frame commands use {CS_M} (primary only) or
 * {CS_M,CS_S} (both) -- bbepT133A01WriteCommandData(bBoth); deep-sleep programs
 * each controller singly, including {CS_S} alone. */
static void tx_cmd_data(uint8_t cmd, const uint8_t *data, size_t len,
                        bool cs_m, bool cs_s)
{
    if (cs_s) gpio_set_level(EPD_PIN_CS_S, 0);
    if (cs_m) gpio_set_level(EPD_PIN_CS_M, 0);

    gpio_set_level(EPD_PIN_DC, 0);
    spi_tx_raw(&cmd, 1);
    if (len) {
        gpio_set_level(EPD_PIN_DC, 1);
        spi_tx_raw(data, len);
    }

    if (cs_m) gpio_set_level(EPD_PIN_CS_M, 1);
    if (cs_s) gpio_set_level(EPD_PIN_CS_S, 1);
}

/* Convenience for the common "primary, optionally both" init form. */
static inline void write_cmd_data(uint8_t cmd, const uint8_t *data, size_t len, bool both)
{
    tx_cmd_data(cmd, data, len, true, both);
}

/* Block until BUSY goes HIGH (idle). UC81xx idle == BUSY high. A full T133A01
 * refresh can take a long time; bb_epaper allows 60 s before giving up, so we
 * only warn past that. */
static void wait_idle(void)
{
    int ticks = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!warned && ++ticks >= 3000) {   /* 3000 * 20 ms = 60 s */
            ESP_LOGW(TAG, "BUSY still low after 60 s -- panel may be stuck");
            warned = true;
        }
    }
}

/* T133A01 update phase (bb_epaper t133a01_update_phase): assert both CS,
 * write cmd+data to both controllers, release primary, wait for BUSY idle
 * while the second CS is still asserted, then release it and settle 30 ms. */
static void update_phase(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(EPD_PIN_CS_S, 0);
    gpio_set_level(EPD_PIN_CS_M, 0);

    gpio_set_level(EPD_PIN_DC, 0);
    spi_tx_raw(&cmd, 1);
    if (len) {
        gpio_set_level(EPD_PIN_DC, 1);
        spi_tx_raw(data, len);
    }

    gpio_set_level(EPD_PIN_CS_M, 1);
    wait_idle();
    gpio_set_level(EPD_PIN_CS_S, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

/* Reset pulse per bbepT133A01InitIO: RST low 20 ms, high 20 ms, wait idle. */
static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    wait_idle();
}

/* Clamp each nibble to a valid Spectra-6 colour (t133a01_panel_byte). Our
 * server already emits panel-native nibbles {0,1,2,3,5,6}, so this is a
 * passthrough in practice; kept for byte-exact parity with the reference. */
static inline uint8_t panel_nibble(uint8_t c)
{
    switch (c & 0x0F) {
    case 0x00: case 0x01: case 0x02:
    case 0x03: case 0x05: case 0x06:
        return c & 0x0F;
    default:
        return 0x00;
    }
}
static inline uint8_t panel_byte(uint8_t packed)
{
    return (panel_nibble(packed >> 4) << 4) | panel_nibble(packed);
}

/* ---------- driver entry points (exported via the vtable below) ---------- */

static esp_err_t t133_port_init(void)
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

    /* Idle: panel powered off, RST high, both CS de-asserted. */
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

/* bbepT133A01InitIO: power-enable, reset, then the fixed command list. The
 * `both` argument on each line is the vendor `bBoth` flag verbatim. bb_epaper
 * delays 10 ms between commands. */
static void t133_init(void)
{
    gpio_set_level(EPD_PIN_PWR, 1);   /* EN: enable panel power */
    vTaskDelay(pdMS_TO_TICKS(10));

    hw_reset();

    #define CMD(op, val, both) do { \
        write_cmd_data((op), (val), sizeof(val), (both)); \
        vTaskDelay(pdMS_TO_TICKS(10)); \
    } while (0)

    CMD(AN_TM,    AN_TM_V,    false);
    CMD(CMD66,    CMD66_V,    true);
    CMD(PSR,      PSR_V,      true);
    CMD(DCDC,     DCDC_V,     false);
    CMD(CDI,      CDI_V,      true);
    CMD(TCON,     TCON_V,     true);
    CMD(AGID,     AGID_V,     true);
    CMD(PWS,      PWS_V,      true);
    CMD(TRES,     TRES_V,     true);
    CMD(PWR,      PWR_V,      false);
    CMD(EN_BUF,   EN_BUF_V,   false);
    CMD(BTST_P,   BTST_P_V,   false);
    CMD(VDDP_EN,  VDDP_EN_V,  false);
    CMD(BTST_N,   BTST_N_V,   false);
    CMD(VDDN,     VDDN_V,     false);
    CMD(VCOM_PWR, VCOM_PWR_V, false);

    #undef CMD

    ESP_LOGI(TAG, "init complete");
}

/* PON -> DRF(0x01) -> POF, each broadcast to both controllers with a BUSY
 * wait in between (bbepRefresh T133A01 branch). */
static void trigger_refresh(void)
{
    update_phase(PON, NULL,  0);
    update_phase(DRF, DRF_V, sizeof(DRF_V));
    update_phase(POF, POF_V, sizeof(POF_V));
    ESP_LOGI(TAG, "refresh done");
}

/* Stream one half-frame (DTM + 300 bytes/row x 1600 rows) to a single CS. */
static void write_half(int cs_pin, const uint8_t *half, size_t len)
{
    gpio_set_level(cs_pin, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    uint8_t cmd = DTM;
    spi_tx_raw(&cmd, 1);
    gpio_set_level(EPD_PIN_DC, 1);
    spi_tx_raw(half, len);
    gpio_set_level(cs_pin, 1);
}

/* bbepWriteImage4bppDual (T133A01, orientation 0): send per-frame CCSET to
 * both controllers, wait idle, then stream the left half to CS_M and the right
 * half to CS_S. Frame layout is identical to the 13.3E6: 600-byte rows, left =
 * bytes [0..299], right = [300..599]. We de-interleave (and colour-clamp) into
 * one reused PSRAM half-buffer, exactly like spectra6_spi_dual. */
static void t133_display(const uint8_t *image)
{
    const size_t HALF_ROW = EPD_WIDTH / 4;          /* 300 */
    const size_t FULL_ROW = HALF_ROW * 2;           /* 600 */
    const size_t HALF_BUF = HALF_ROW * EPD_HEIGHT;  /* 480000 */

    uint8_t *scratch = heap_caps_malloc(HALF_BUF, MALLOC_CAP_SPIRAM);
    if (!scratch) {
        ESP_LOGE(TAG, "OOM allocating %u-byte display scratch", (unsigned)HALF_BUF);
        return;
    }

    /* Per-frame "current frame" CCSET (0xE0,0x01) to both controllers. */
    write_cmd_data(CCSET, CCSET_V, sizeof(CCSET_V), true);
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Left half -> primary (CS_M) */
    for (size_t r = 0; r < EPD_HEIGHT; r++) {
        const uint8_t *src = image + r * FULL_ROW;
        uint8_t *dst = scratch + r * HALF_ROW;
        for (size_t x = 0; x < HALF_ROW; x++) dst[x] = panel_byte(src[x]);
    }
    write_half(EPD_PIN_CS_M, scratch, HALF_BUF);

    /* Right half -> second (CS_S) */
    for (size_t r = 0; r < EPD_HEIGHT; r++) {
        const uint8_t *src = image + r * FULL_ROW + HALF_ROW;
        uint8_t *dst = scratch + r * HALF_ROW;
        for (size_t x = 0; x < HALF_ROW; x++) dst[x] = panel_byte(src[x]);
    }
    write_half(EPD_PIN_CS_S, scratch, HALF_BUF);

    free(scratch);
    trigger_refresh();
}

static void t133_clear(uint8_t color)
{
    const size_t HALF = EPD_BUF_BYTES / 2;
    uint8_t *buf = heap_caps_malloc(HALF, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte clear buffer", (unsigned)HALF);
        return;
    }
    uint8_t packed = panel_byte((color << 4) | color);
    memset(buf, packed, HALF);

    write_cmd_data(CCSET, CCSET_V, sizeof(CCSET_V), true);
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(10));

    write_half(EPD_PIN_CS_M, buf, HALF);
    write_half(EPD_PIN_CS_S, buf, HALF);

    free(buf);
    trigger_refresh();
}

/* Diagnostic: six palette bands, full width. Streams one 300-byte half-row per
 * band repeated N times, matching spectra6_spi_dual's approach. */
static void t133_show_color_bars(void)
{
    static const uint8_t palette[6] = {
        EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_YELLOW,
        EPD_COL_RED,   EPD_COL_BLUE,   EPD_COL_GREEN,
    };
    const size_t HALF_ROW = EPD_WIDTH / 4;     /* 300 */
    uint8_t row[HALF_ROW];

    write_cmd_data(CCSET, CCSET_V, sizeof(CCSET_V), true);
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(10));

    for (int side = 0; side < 2; side++) {
        int cs = (side == 0) ? EPD_PIN_CS_M : EPD_PIN_CS_S;
        gpio_set_level(cs, 0);
        gpio_set_level(EPD_PIN_DC, 0);
        uint8_t cmd = DTM;
        spi_tx_raw(&cmd, 1);
        gpio_set_level(EPD_PIN_DC, 1);
        for (int b = 0; b < 6; b++) {
            uint8_t packed = panel_byte((palette[b] << 4) | palette[b]);
            memset(row, packed, HALF_ROW);
            size_t band_h = EPD_HEIGHT / 6;
            if (b == 5) band_h += EPD_HEIGHT % 6;
            for (size_t r = 0; r < band_h; r++) spi_tx_raw(row, HALF_ROW);
        }
        gpio_set_level(cs, 1);
    }
    trigger_refresh();
}

static void t133_show_palette_sweep(void)
{
    const size_t HALF_ROW = EPD_WIDTH / 4;     /* 300 */
    const size_t BAND_H   = EPD_HEIGHT / 8;    /* 200 */
    uint8_t row[HALF_ROW];

    write_cmd_data(CCSET, CCSET_V, sizeof(CCSET_V), true);
    wait_idle();
    vTaskDelay(pdMS_TO_TICKS(10));

    for (int side = 0; side < 2; side++) {
        int cs = (side == 0) ? EPD_PIN_CS_M : EPD_PIN_CS_S;
        gpio_set_level(cs, 0);
        gpio_set_level(EPD_PIN_DC, 0);
        uint8_t cmd = DTM;
        spi_tx_raw(&cmd, 1);
        gpio_set_level(EPD_PIN_DC, 1);
        for (uint8_t n = 0; n < 8; n++) {
            uint8_t packed = (n << 4) | n;   /* raw sweep: do NOT clamp */
            memset(row, packed, HALF_ROW);
            for (size_t r = 0; r < BAND_H; r++) spi_tx_raw(row, HALF_ROW);
        }
        gpio_set_level(cs, 1);
    }
    trigger_refresh();
}

/* bbepSleep (T133A01): power off both controllers, wait idle, deep-sleep both
 * (0x07,0xA5), then drop the power rail. */
static void t133_sleep(void)
{
    tx_cmd_data(POF, POF_V, sizeof(POF_V), true,  false);  /* primary (CS_M) */
    tx_cmd_data(POF, POF_V, sizeof(POF_V), false, true);   /* second  (CS_S) */
    wait_idle();

    uint8_t magic = 0xA5;
    tx_cmd_data(DEEP_SLEEP, &magic, 1, true,  false);      /* primary (CS_M) */
    tx_cmd_data(DEEP_SLEEP, &magic, 1, false, true);       /* second  (CS_S) */

    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(EPD_PIN_PWR, 0);   /* EN low: cut panel power */
    gpio_set_level(EPD_PIN_RST, 0);
}

/* ---------- exported vtable ---------- */

const epd_driver_t spectra6_t133a01_dual_driver = {
    .info = {
        .name      = "Spectra-6 13.3\" T133A01 dual (1200x1600)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = t133_port_init,
    .init               = t133_init,
    .clear              = t133_clear,
    .display            = t133_display,
    .show_color_bars    = t133_show_color_bars,
    .show_palette_sweep = t133_show_palette_sweep,
    .sleep              = t133_sleep,
};

#endif /* PANEL_DRIVER_SPECTRA6_T133A01_DUAL */
