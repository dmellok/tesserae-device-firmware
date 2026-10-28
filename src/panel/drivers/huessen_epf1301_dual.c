/*
 * Huessen EPF1301 -- 13.3" EL133UF1 dual-chip Spectra-6 (1200x1600).
 *
 * The glass, the register values and the frame layout are the same family as
 * spectra6_t133a01_dual (the Inkplate 13SPECTRA runs this exact panel), but the
 * carrier is wired and sequenced differently enough that it gets its own
 * driver instead of a fourth set of #ifdefs there:
 *
 *   - NO DC LINE. The controllers are strapped for 3-wire SPI and take the
 *     first byte of each chip-select frame as the opcode, the rest as its
 *     parameters (the same framing as the OpenPaper L's EL133UF3). Gaps inside
 *     a frame are fine; the vendor firmware sends DTM in 4800-byte chunks.
 *   - The two chip-selects are plain GPIOs (CTRL1 / CTRL2, low = selected).
 *     The vendor's SPI hardware CS sits on GPIO0, a boot strap, and is not
 *     connected to the panel, so this driver never touches it.
 *   - The display rail (SYS_POWER, GPIO17 = EPD_PIN_PWR) is cycled from cold
 *     around every paint: 1 s off, on, two resets 1 s apart, init, data,
 *     refresh, then a slow teardown that bleeds the signal lines and puts the
 *     controllers in reset before the rail goes. A shorter, warmer sequence
 *     can wedge the controllers ("half-rendered, only a factory reflash
 *     clears it"), so this one is followed step for step.
 *   - The chip-selects are released BEFORE each BUSY wait (both high), not
 *     held across it the way the T133A01 boards do.
 *   - CCSET (0xE0) and AN_TM go to both controllers inside init; there is no
 *     per-frame CCSET.
 *   - BUSY has an external pull-up on the PCB. The internal pull-up must stay
 *     off (it masks the controller's BUSY low) and the pad must never go
 *     through gpio_reset_pin(), which turns that pull-up on.
 *
 * Every value below follows the stock E_Frame v2.0.26 firmware. Confirmed on
 * hardware: a healthy DRF takes ~19 s, and the selftest bars and server frames
 * paint correctly across both controller halves.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_HUESSEN_EPF1301_DUAL)

#include "drivers/huessen_epf1301_dual.h"
#include "../busy_sleep.h"
#include "diag_run.h"           /* BUSY timeouts latch a report for the next status */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_huessen";

/* --- UC81xx-class opcodes used by this panel --- */
#define PSR         0x00
#define PWR         0x01
#define POF         0x02
#define PON         0x04
#define BTST_N      0x05
#define BTST_P      0x06
#define DTM         0x10
#define DRF         0x12
#define PLL         0x30
#define CDI         0x50
#define TCON        0x60
#define TRES        0x61
#define AN_TM       0x74
#define AGID        0x86
#define VDDN        0xB0
#define VCOM_PWR    0xB1
#define EN_BUF      0xB6
#define VDDP_EN     0xB7
#define CCSET       0xE0
#define PWS         0xE3
#define CMD66       0xF0

/* --- Init blobs: stock v2.0.26 (June 2025) sequence, do NOT edit --- */
static const uint8_t AN_TM_V[]    = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
static const uint8_t CMD66_V[]    = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t PSR_V[]      = {0xDF, 0x6B};
static const uint8_t PLL_V[]      = {0x08};
static const uint8_t CDI_V[]      = {0xF7};
static const uint8_t TCON_V[]     = {0x03, 0x03};
static const uint8_t AGID_V[]     = {0x10};
static const uint8_t PWS_V[]      = {0x22};
static const uint8_t CCSET_V[]    = {0x01};
static const uint8_t TRES_V[]     = {0x04, 0xB0, 0x03, 0x20};   /* 1200 x 800 logical */
static const uint8_t PWR_V[]      = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t EN_BUF_V[]   = {0x07};
static const uint8_t BTST_P_V[]   = {0xD8, 0x18};
static const uint8_t VDDP_EN_V[]  = {0x01};
static const uint8_t BTST_N_V[]   = {0xD8, 0x18};
static const uint8_t VDDN_V[]     = {0x01};
static const uint8_t VCOM_PWR_V[] = {0x02};
static const uint8_t DRF_V[]      = {0x00};
static const uint8_t POF_V[]      = {0x00};

/* Vendor timing. */
#define RAIL_OFF_MS          1000   /* rail low before power-up: bulk cap decay */
#define RAIL_SETTLE_MS       10
#define RST_MS               100    /* RST low, then high, each this long */
#define RESET_GAP_MS         1000   /* DC-DC settle between the two resets */
#define DRF_PRE_DELAY_MS     30
#define TEARDOWN_BLEED_MS    1000   /* signal lines low before the rail drops */

/* A healthy dual-panel refresh takes ~19 s. One that "finishes" faster than
 * this never ran: the controller is not answering and BUSY only reads high
 * off the board pull-up. */
#define DRF_MIN_PLAUSIBLE_MS 5000

#define BUSY_IDLE_STABLE_SAMPLES 3
#define BUSY_POLL_MS             20
#define BUSY_ASSERT_TIMEOUT_MS   1000
#define BUSY_RESET_TIMEOUT_MS    5000
#define BUSY_PON_TIMEOUT_MS      5000
#define BUSY_DRF_TIMEOUT_MS      60000
#define BUSY_POF_TIMEOUT_MS      5000
#define BUSY_WARN_MS             30000

/* Panel data goes out through one internal DMA buffer of whole half-rows: 16
 * rows of 300 bytes, the vendor's 4800-byte transfer. Streaming from it avoids
 * a 480 KB PSRAM scratch per paint. +1 for the DTM opcode in the first chunk. */
#define HALF_ROW        (EPD_WIDTH / 4)     /* 300 bytes = 600 px */
#define FULL_ROW        (HALF_ROW * 2)      /* 600 bytes = 1200 px */
#define CHUNK_ROWS      16
#define CHUNK_BYTES     (HALF_ROW * CHUNK_ROWS)

static spi_device_handle_t s_spi;
static bool s_port_inited = false;

/* ---------- low-level pin/SPI wrappers ---------- */

static esp_err_t spi_tx_raw(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = len * 8;
    t.tx_buffer = data;
    esp_err_t err = spi_device_polling_transmit(s_spi, &t);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "spi tx (%u B) failed: %s", (unsigned)len, esp_err_to_name(err));
    return err;
}

static inline void cs_select(bool cs_m, bool cs_s)
{
    if (cs_m) gpio_set_level(EPD_PIN_CS_M, 0);
    if (cs_s) gpio_set_level(EPD_PIN_CS_S, 0);
}

static inline void cs_release(void)
{
    gpio_set_level(EPD_PIN_CS_M, 1);
    gpio_set_level(EPD_PIN_CS_S, 1);
}

/* One command frame: opcode then parameters in a single transfer, framed by
 * the requested chip-select(s). Every parameter list here is < 16 bytes. */
static void write_cmd(uint8_t cmd, const uint8_t *data, size_t len, bool cs_m, bool cs_s)
{
    uint8_t buf[16];
    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    buf[0] = cmd;
    if (len) memcpy(buf + 1, data, len);
    cs_select(cs_m, cs_s);
    spi_tx_raw(buf, len + 1);
    cs_release();
}

#define CMD_BOTH(op, val)     write_cmd((op), (val), sizeof(val), true, true)
#define CMD_PRIMARY(op, val)  write_cmd((op), (val), sizeof(val), true, false)

/* Wait for BUSY to read high on BUSY_IDLE_STABLE_SAMPLES consecutive samples
 * or for `timeout_ms` to elapse. Returns false on timeout; callers carry on so
 * the panel still reaches POF and the rail still drops. */
static bool wait_idle(const char *tag, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    int high = 0;
    bool warned = false;
    while (high < BUSY_IDLE_STABLE_SAMPLES) {
        if (gpio_get_level(EPD_PIN_BUSY) == 0) {
            high = 0;
            waited += epd_busy_sleep(EPD_PIN_BUSY, 1, BUSY_POLL_MS);
        } else {
            high++;
            vTaskDelay(pdMS_TO_TICKS(BUSY_POLL_MS));
            waited += BUSY_POLL_MS;
        }
        if (!warned && waited >= BUSY_WARN_MS && timeout_ms > BUSY_WARN_MS) {
            ESP_LOGW(TAG, "%s: BUSY still low after %u s; panel may be slow or stuck",
                     tag, (unsigned)(BUSY_WARN_MS / 1000));
            warned = true;
        }
        if (waited >= timeout_ms) {
            ESP_LOGE(TAG, "%s: BUSY never cleared in %u ms; continuing so the "
                          "panel is still powered down rather than left driven",
                     tag, (unsigned)timeout_ms);
            diag_note_paint(DIAG_PAINT_READY_TIMEOUT);
            return false;
        }
    }
    return true;
}

/* Wait for the controller to pull BUSY low after DRF, so an idle wait that
 * starts before it has does not end at once and send POF mid-waveform.
 * Bounded. Not used for PON / POF, whose BUSY pulse can fall between polls. */
static bool wait_busy_asserted(void)
{
    for (int waited = 0; waited < BUSY_ASSERT_TIMEOUT_MS; waited += 2) {
        if (gpio_get_level(EPD_PIN_BUSY) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static void busy_as_input(void)
{
    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
        .pull_up_en = GPIO_PULLUP_DISABLE,      /* the PCB has its own pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&in);
}

/* RST low then high, RST_MS each, then wait for the controllers to come up. */
static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(RST_MS));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(RST_MS));
    wait_idle("reset", BUSY_RESET_TIMEOUT_MS);
}

/* Clamp each nibble to one of the six inks; the server already emits only
 * these, so in practice this is a passthrough. */
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

/* ---------- half-panel data streaming ---------- */

/* One controller's 480000 bytes go out as DTM + 1600 rows of 300 bytes. The
 * caller fills the chunk buffer a row at a time through half_row(); it is
 * flushed every CHUNK_ROWS rows, with the opcode in front of the first one. */
typedef struct {
    uint8_t *buf;       /* 1 + CHUNK_BYTES, internal DMA-capable */
    size_t   fill;      /* bytes queued, including the opcode if still pending */
    int      cs_pin;
} half_tx_t;

static bool half_begin(half_tx_t *h, int cs_pin)
{
    h->buf = heap_caps_malloc(1 + CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!h->buf) {
        ESP_LOGE(TAG, "OOM allocating %u-byte DMA chunk", (unsigned)(1 + CHUNK_BYTES));
        return false;
    }
    h->cs_pin = cs_pin;
    h->buf[0] = DTM;
    h->fill = 1;
    gpio_set_level(cs_pin, 0);
    return true;
}

static void half_flush(half_tx_t *h)
{
    spi_tx_raw(h->buf, h->fill);
    h->fill = 0;
}

static void half_row(half_tx_t *h, const uint8_t *src, bool clamp)
{
    uint8_t *dst = h->buf + h->fill;
    if (clamp) {
        for (size_t x = 0; x < HALF_ROW; x++) dst[x] = panel_byte(src[x]);
    } else {
        memcpy(dst, src, HALF_ROW);
    }
    h->fill += HALF_ROW;
    if (h->fill + HALF_ROW > 1 + CHUNK_BYTES) half_flush(h);
}

static void half_end(half_tx_t *h)
{
    if (h->fill) half_flush(h);
    gpio_set_level(h->cs_pin, 1);
    free(h->buf);
    h->buf = NULL;
}

/* ---------- refresh ---------- */

/* PON -> DRF -> POF, each broadcast to both controllers, the chip-selects
 * released before every BUSY wait. Each phase runs whether or not the one
 * before timed out, so POF always goes out. */
static void trigger_refresh(void)
{
    write_cmd(PON, NULL, 0, true, true);
    bool ok = wait_idle("PON", BUSY_PON_TIMEOUT_MS);

    uint8_t drf[1 + sizeof(DRF_V)] = {DRF};
    memcpy(drf + 1, DRF_V, sizeof(DRF_V));
    cs_select(true, true);
    vTaskDelay(pdMS_TO_TICKS(DRF_PRE_DELAY_MS));
    spi_tx_raw(drf, sizeof(drf));
    cs_release();
    int64_t t0 = esp_timer_get_time();
    if (!wait_busy_asserted())
        ESP_LOGW(TAG, "DRF: panel never asserted BUSY within %d ms; waiting for idle anyway",
                 BUSY_ASSERT_TIMEOUT_MS);
    ok = wait_idle("DRF", BUSY_DRF_TIMEOUT_MS) && ok;
    int64_t drf_ms = (esp_timer_get_time() - t0) / 1000;
    if (drf_ms < DRF_MIN_PLAUSIBLE_MS) {
        /* Do NOT retry or reboot: a wedged controller is not cleared by a
         * reset, only by a real power cycle, so a retry loop would burn the
         * battery on refreshes that never happen. */
        ESP_LOGE(TAG, "DRF finished in %lld ms (< %d ms): the controllers are not "
                      "responding and the glass was not refreshed",
                 (long long)drf_ms, DRF_MIN_PLAUSIBLE_MS);
        diag_note_paint(DIAG_PAINT_INIT_FAILED);
        ok = false;
    } else {
        ESP_LOGI(TAG, "DRF done in %lld ms", (long long)drf_ms);
    }

    write_cmd(POF, POF_V, sizeof(POF_V), true, true);
    ok = wait_idle("POF", BUSY_POF_TIMEOUT_MS) && ok;

    if (ok) ESP_LOGI(TAG, "refresh done");
    else    ESP_LOGE(TAG, "refresh finished with an error; frame may be incomplete");
}

/* ---------- driver entry points (exported via the vtable below) ---------- */

static esp_err_t huessen_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    /* The board parks the rail low and holds it through deep sleep
     * (BOARD_SLEEP_DRIVE_LOW_MASK); release every hold before driving. */
    const int pins[] = { EPD_PIN_CS_M, EPD_PIN_CS_S, EPD_PIN_RST, EPD_PIN_PWR, EPD_PIN_BUSY };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        gpio_hold_dis((gpio_num_t)pins[i]);

    /* Levels before directions, so no output glitches low on enable: a low
     * chip-select during bus init would hand SCLK/MOSI noise to the panel as
     * commands. Idle = both deselected, RST high, rail off. */
    gpio_set_level(EPD_PIN_CS_M, 1);
    gpio_set_level(EPD_PIN_CS_S, 1);
    gpio_set_level(EPD_PIN_RST,  1);
    gpio_set_level(EPD_PIN_PWR,  0);
    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_CS_M) | (1ULL << EPD_PIN_CS_S) |
                        (1ULL << EPD_PIN_RST)  | (1ULL << EPD_PIN_PWR),
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    busy_as_input();

    spi_bus_config_t bus = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_PIN_MOSI,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 1 + CHUNK_BYTES,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,            /* CTRL1/CTRL2 by hand; GPIO0 left alone */
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

/* Cold power-up, two resets, then the 17-command init: AN_TM..TRES to both
 * controllers, PWR..VCOM_PWR to the primary only. */
static void huessen_init(void)
{
    /* The previous teardown left BUSY an output driven low; read our own pin
     * and every wait below would time out. */
    busy_as_input();

    /* The rail may still be up (two paints in one wake) or only just down:
     * hold it off long enough for the boost rail to decay, so the
     * controllers start from cold every time. */
    gpio_set_level(EPD_PIN_PWR, 0);
    vTaskDelay(pdMS_TO_TICKS(RAIL_OFF_MS));
    gpio_set_level(EPD_PIN_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(RAIL_SETTLE_MS));
    cs_release();
    ESP_LOGI(TAG, "rail on: BUSY=%d", gpio_get_level(EPD_PIN_BUSY));

    hw_reset();
    vTaskDelay(pdMS_TO_TICKS(RESET_GAP_MS));
    hw_reset();

    CMD_BOTH(AN_TM, AN_TM_V);
    CMD_BOTH(CMD66, CMD66_V);
    CMD_BOTH(PSR,   PSR_V);
    CMD_BOTH(PLL,   PLL_V);
    CMD_BOTH(CDI,   CDI_V);
    CMD_BOTH(TCON,  TCON_V);
    CMD_BOTH(AGID,  AGID_V);
    CMD_BOTH(PWS,   PWS_V);
    CMD_BOTH(CCSET, CCSET_V);
    CMD_BOTH(TRES,  TRES_V);

    CMD_PRIMARY(PWR,      PWR_V);
    CMD_PRIMARY(EN_BUF,   EN_BUF_V);
    CMD_PRIMARY(BTST_P,   BTST_P_V);
    CMD_PRIMARY(VDDP_EN,  VDDP_EN_V);
    CMD_PRIMARY(BTST_N,   BTST_N_V);
    CMD_PRIMARY(VDDN,     VDDN_V);
    CMD_PRIMARY(VCOM_PWR, VCOM_PWR_V);

    ESP_LOGI(TAG, "init complete");
}

/* Frame layout is the shared 13.3" one: 1600 rows of 600 packed bytes, the
 * left 300 (columns 0..599) to the primary (CTRL1), the right 300 to the
 * second (CTRL2) -- the same split the vendor firmware sends as two 480 KB
 * blocks. */
static void huessen_display(const uint8_t *image)
{
    for (int side = 0; side < 2; side++) {
        half_tx_t h;
        if (!half_begin(&h, side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S)) return;
        for (size_t r = 0; r < EPD_HEIGHT; r++)
            half_row(&h, image + r * FULL_ROW + side * HALF_ROW, true);
        half_end(&h);
    }
    trigger_refresh();
}

static void huessen_clear(uint8_t color)
{
    uint8_t row[HALF_ROW];
    memset(row, panel_byte((color << 4) | color), HALF_ROW);
    for (int side = 0; side < 2; side++) {
        half_tx_t h;
        if (!half_begin(&h, side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S)) return;
        for (size_t r = 0; r < EPD_HEIGHT; r++) half_row(&h, row, false);
        half_end(&h);
    }
    trigger_refresh();
}

/* Diagnostic: six palette bands top to bottom, full width. */
static void huessen_show_color_bars(void)
{
    static const uint8_t palette[6] = {
        EPD_COL_BLACK, EPD_COL_WHITE,  EPD_COL_YELLOW,
        EPD_COL_RED,   EPD_COL_BLUE,   EPD_COL_GREEN,
    };
    uint8_t row[HALF_ROW];
    for (int side = 0; side < 2; side++) {
        half_tx_t h;
        if (!half_begin(&h, side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S)) return;
        for (int b = 0; b < 6; b++) {
            memset(row, panel_byte((palette[b] << 4) | palette[b]), HALF_ROW);
            size_t band_h = EPD_HEIGHT / 6;
            if (b == 5) band_h += EPD_HEIGHT % 6;
            for (size_t r = 0; r < band_h; r++) half_row(&h, row, false);
        }
        half_end(&h);
    }
    trigger_refresh();
}

/* Diagnostic: raw nibbles 0..7 as eight bands, unclamped. */
static void huessen_show_palette_sweep(void)
{
    const size_t BAND_H = EPD_HEIGHT / 8;    /* 200 */
    uint8_t row[HALF_ROW];
    for (int side = 0; side < 2; side++) {
        half_tx_t h;
        if (!half_begin(&h, side == 0 ? EPD_PIN_CS_M : EPD_PIN_CS_S)) return;
        for (uint8_t n = 0; n < 8; n++) {
            memset(row, (n << 4) | n, HALF_ROW);
            for (size_t r = 0; r < BAND_H; r++) half_row(&h, row, false);
        }
        half_end(&h);
    }
    trigger_refresh();
}

/* Vendor teardown. No deep-sleep command: the rail goes instead. BUSY is
 * driven low with the bus idle (SCLK idles low in mode 0) and everything is
 * held for TEARDOWN_BLEED_MS, so no line back-biases the controllers through
 * their ESD diodes; then both chip-selects and RST go low, putting the
 * controllers in reset BEFORE the rail drops so they cannot latch up in the
 * brown-out. init() turns BUSY back into an input. */
static void huessen_sleep(void)
{
    gpio_set_direction(EPD_PIN_BUSY, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PIN_BUSY, 0);
    vTaskDelay(pdMS_TO_TICKS(TEARDOWN_BLEED_MS));

    gpio_set_level(EPD_PIN_CS_M, 0);
    gpio_set_level(EPD_PIN_CS_S, 0);
    gpio_set_level(EPD_PIN_RST,  0);
    gpio_set_level(EPD_PIN_PWR,  0);
    ESP_LOGI(TAG, "panel off");
}

/* ---------- exported vtable ---------- */

const epd_driver_t huessen_epf1301_dual_driver = {
    .info = {
        .name      = "Spectra-6 13.3\" EL133UF1 dual, Huessen EPF1301 (1200x1600)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
        .grayscale = false,    /* nibbles are indices into 6 fixed colours */
    },
    .port_init          = huessen_port_init,
    .init               = huessen_init,
    .clear              = huessen_clear,
    .display            = huessen_display,
    .show_color_bars    = huessen_show_color_bars,
    .show_palette_sweep = huessen_show_palette_sweep,
    .sleep              = huessen_sleep,
};

#endif /* PANEL_DRIVER_HUESSEN_EPF1301_DUAL */
