/*
 * Seeed reTerminal E1003 -- 10.3" ED103TC2 grayscale via IT8951, Family D.
 *
 * Ported from bitbank2 FastEPD's IT8951 path (initIT8951 + setPanelSize(BBEP_DISPLAY_ED103TC2)). The IT8951 is a
 * timing controller reached over SPI with a 16-bit-word protocol:
 *   - write command: preamble 0x6000, then the 16-bit command
 *   - write data:    preamble 0x0000, then 16-bit data word(s)
 *   - read data:     preamble 0x1000, a dummy word, then read 16-bit word(s)
 * Every transfer waits on HRDY (the BUSY line, HIGH = ready -- opposite of the
 * UC81xx panels). Command/data is framed by CS, not a DC pin.
 *
 * Flow: power up + reset, SYS_RUN, set VCOM, GET_DEV_INFO (reads the image-
 * buffer address), then per frame: load the 4bpp buffer into the controller's
 * DRAM at that address (LD_IMG_AREA), and trigger a GC16 grayscale refresh
 * (DPY_AREA mode 2). The ED103TC2 mirrors X, so each row is reversed and its
 * nibbles swapped before streaming, exactly as FastEPD does.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_IT8951_GRAY)

#include "drivers/it8951_gray.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_it8951";

/* IT8951 commands */
#define SYS_RUN       0x0001
#define STANDBY       0x0002
#define SLEEP         0x0003
#define REG_RD        0x0010
#define REG_WR        0x0011
#define LD_IMG_AREA   0x0021
#define LD_IMG_END    0x0022
#define DPY_AREA      0x0034
#define GET_DEV_INFO  0x0302
#define VCOM          0x0039
#define CMD_TEMP      0x0040   /* force temperature (selects the waveform LUT) */

/* IT8951 registers */
#define REG_I80CPCR   0x0004   /* host-command packed-write enable */
#define REG_LISAR     0x0208   /* image buffer start addr (low); +2 = high */
#define REG_LUTAFSR   0x1224   /* LUT engine status: 0 = idle/done */
#define REG_UP1SR     0x1138   /* update param; +2 has the 1bpp-mode bit */

/* LD_IMG_AREA info-word fields */
#define PIXFMT_4BPP   2
#define ENDIAN_L      0
#define ENDIAN_B      1
#define MODE_GC16     2        /* 16-level grayscale waveform */

static spi_device_handle_t s_spi;
static bool     s_port_inited = false;
static uint32_t s_img_buf_addr;

/* ---------- SPI framing (16-bit words, MSB-first) ---------- */

static inline void cs(int level) { gpio_set_level(EPD_PIN_CS, level); }

/* HRDY: wait while BUSY is LOW (controller busy). ~3 s cap. */
static void wait_ready(void)
{
    int n = 0;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (++n > 3000) { ESP_LOGW(TAG, "HRDY timeout"); return; }
    }
}

static void word_tx(uint16_t w)
{
    uint8_t b[2] = { (uint8_t)(w >> 8), (uint8_t)w };
    spi_transaction_t t = {0};
    t.length = 16; t.tx_buffer = b;
    spi_device_polling_transmit(s_spi, &t);
}

static uint16_t word_rx(void)
{
    uint8_t tx[2] = {0, 0}, rx[2] = {0, 0};
    spi_transaction_t t = {0};
    t.length = 16; t.tx_buffer = tx; t.rxlength = 16; t.rx_buffer = rx;
    spi_device_polling_transmit(s_spi, &t);
    return (uint16_t)((rx[0] << 8) | rx[1]);
}

static void write_cmd(uint16_t cmd)
{
    cs(0);
    wait_ready(); word_tx(0x6000);
    wait_ready(); word_tx(cmd);
    cs(1);
}

static void write_data(uint16_t data)
{
    cs(0);
    wait_ready(); word_tx(0x0000);
    wait_ready(); word_tx(data);
    cs(1);
}

static uint16_t read_data(void)
{
    cs(0);
    wait_ready(); word_tx(0x1000);
    word_tx(0x0000);              /* dummy */
    wait_ready();
    uint16_t v = word_rx();
    cs(1);
    return v;
}

static void read_ndata(uint16_t *buf, int n)
{
    cs(0);
    wait_ready(); word_tx(0x1000);
    wait_ready(); word_tx(0x0000);   /* dummy */
    wait_ready();
    for (int i = 0; i < n; i++) buf[i] = word_rx();
    cs(1);
}

static void send_cmd_args(uint16_t cmd, const uint16_t *args, int n)
{
    write_cmd(cmd);
    for (int i = 0; i < n; i++) write_data(args[i]);
}

static void write_reg(uint16_t addr, uint16_t val)
{
    write_cmd(REG_WR); write_data(addr); write_data(val);
}

static uint16_t read_reg(uint16_t addr)
{
    write_cmd(REG_RD); write_data(addr);
    return read_data();
}

/* Poll the LUT engine until the refresh completes. ~30 s cap. */
static void wait_display_done(void)
{
    int n = 0;
    while (read_reg(REG_LUTAFSR) != 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++n > 3000) { ESP_LOGW(TAG, "display (LUTAFSR) timeout"); return; }
    }
}

/* ---------- power ---------- */

static void hw_reset_and_power(void)
{
    /* Power-cycle the two rails, then pulse RST. */
    gpio_set_level(EPD_PIN_EN, 0);
    gpio_set_level(EPD_PIN_VCC_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(EPD_PIN_EN, 1);
    gpio_set_level(EPD_PIN_VCC_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    wait_ready();
}

/* ---------- driver entry points ---------- */

static esp_err_t it8951_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_CS) | (1ULL << EPD_PIN_RST) |
                        (1ULL << EPD_PIN_EN) | (1ULL << EPD_PIN_VCC_EN),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << EPD_PIN_BUSY),
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_RST, 1);
    gpio_set_level(EPD_PIN_EN, 1);
    gpio_set_level(EPD_PIN_VCC_EN, 1);

    spi_bus_config_t bus = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = EPD_PIN_MISO,       /* bidirectional */
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = EPD_SPI_HZ,
        .mode = 0,
        .spics_io_num = -1,                /* manual CS */
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

static void it8951_init(void)
{
    hw_reset_and_power();

    write_cmd(SYS_RUN);

    /* GET_DEV_INFO -> panelW, panelH, imgBufAddrL, imgBufAddrH, fw[8], lut[8]. */
    uint16_t info[20] = {0};
    write_cmd(GET_DEV_INFO);
    read_ndata(info, 20);
    s_img_buf_addr = ((uint32_t)info[3] << 16) | info[2];

    /* Tail of FastEPD's bbepInitIT8951: set VCOM, enable packed-write, force a
     * temperature. Packed-write is required for the byte-stream image load to be
     * interpreted correctly; the forced temperature selects a waveform LUT (with
     * neither, the panel accepts everything but never develops -- stays blank). */
    write_cmd(VCOM); write_data(0x0001); write_data(EPD_VCOM_MV);
    write_reg(REG_I80CPCR, 0x0001);                              /* packed write */
    write_cmd(CMD_TEMP); write_data(0x0001); write_data(14);     /* force 14 C */

    ESP_LOGI(TAG, "init complete: dev %ux%u, img_buf=0x%08x",
             info[0], info[1], (unsigned)s_img_buf_addr);
}

static void load_img_area_start(void)
{
    /* target address (LISAR) */
    write_reg(REG_LISAR + 2, (uint16_t)((s_img_buf_addr >> 16) & 0xFFFF));
    write_reg(REG_LISAR,     (uint16_t)(s_img_buf_addr & 0xFFFF));

    /* ensure 1bpp mode is off (we use 4bpp) */
    write_reg(REG_UP1SR + 2, (uint16_t)(read_reg(REG_UP1SR + 2) & ~(1 << 2)));

    /* info word: MIRROR_X -> big-endian; 4bpp; rotate 0. Then x,y,w,h. */
    uint16_t args[5] = {
        (uint16_t)((ENDIAN_B << 8) | (PIXFMT_4BPP << 4) | 0),
        0, 0, EPD_WIDTH, EPD_HEIGHT,
    };
    send_cmd_args(LD_IMG_AREA, args, 5);
}

/* Stream one 4bpp row (936 bytes) with the ED103TC2 MIRROR_X transform: reverse
 * the byte order and swap the two nibbles in each byte (so pixels fully reverse
 * left-to-right). `scratch` is a caller-owned iPitch buffer. */
static void stream_row_mirrored(const uint8_t *row, uint8_t *scratch)
{
    const int iPitch = EPD_WIDTH / 2;   /* 936 */
    for (int x = 0; x < iPitch; x++) {
        uint8_t b = row[x];
        scratch[iPitch - 1 - x] = (uint8_t)((b >> 4) | (b << 4));
    }
    spi_transaction_t t = {0};
    t.length = iPitch * 8; t.tx_buffer = scratch;
    spi_device_polling_transmit(s_spi, &t);
}

/* Load a full-frame 4bpp buffer into controller DRAM and GC16-refresh it. */
static void write_and_refresh(const uint8_t *fb)
{
    const int iPitch = EPD_WIDTH / 2;   /* 936 bytes/row */
    uint8_t *scratch = heap_caps_malloc(iPitch, MALLOC_CAP_DMA);
    if (!scratch) { ESP_LOGE(TAG, "OOM row scratch"); return; }

    write_cmd(SYS_RUN);            /* ensure powered */
    wait_display_done();
    load_img_area_start();

    cs(0);
    wait_ready(); word_tx(0x0000);   /* data preamble */
    wait_ready();
    const uint8_t *s = fb;
    for (int y = 0; y < EPD_HEIGHT; y++) {
        stream_row_mirrored(s, scratch);
        s += iPitch;
    }
    cs(1);

    write_cmd(LD_IMG_END);

    uint16_t dargs[5] = { 0, 0, EPD_WIDTH, EPD_HEIGHT, MODE_GC16 };
    send_cmd_args(DPY_AREA, dargs, 5);
    wait_display_done();

    write_cmd(STANDBY);           /* rest the panel power between updates */
    free(scratch);
    ESP_LOGI(TAG, "refresh done");
}

static void it8951_display(const uint8_t *image)
{
    write_and_refresh(image);
}

/* Fill the whole panel with one gray level (nibble). */
static void it8951_clear(uint8_t color)
{
    const int iPitch = EPD_WIDTH / 2;
    uint8_t *row = heap_caps_malloc(iPitch, MALLOC_CAP_DMA);
    if (!row) return;
    uint8_t nib = color & 0x0F;
    memset(row, (nib << 4) | nib, iPitch);

    write_cmd(SYS_RUN);
    wait_display_done();
    load_img_area_start();
    cs(0);
    wait_ready(); word_tx(0x0000);
    wait_ready();
    spi_transaction_t t = {0};
    t.length = iPitch * 8; t.tx_buffer = row;
    for (int y = 0; y < EPD_HEIGHT; y++) spi_device_polling_transmit(s_spi, &t);
    cs(1);
    write_cmd(LD_IMG_END);
    uint16_t dargs[5] = { 0, 0, EPD_WIDTH, EPD_HEIGHT, MODE_GC16 };
    send_cmd_args(DPY_AREA, dargs, 5);
    wait_display_done();
    write_cmd(STANDBY);
    free(row);
    ESP_LOGI(TAG, "clear done");
}

/* Diagnostic: 16 horizontal bands, black -> white gray ramp. A healthy panel
 * shows a smooth grayscale gradient; banding/blotches point at the waveform or
 * the load. (Horizontal bands are mirror-invariant, so no per-row reversal.) */
static void it8951_show_color_bars(void)
{
    const int iPitch = EPD_WIDTH / 2;
    const int BAND_H = EPD_HEIGHT / 16;   /* ~87 rows/band */
    uint8_t *row = heap_caps_malloc(iPitch, MALLOC_CAP_DMA);
    if (!row) return;

    write_cmd(SYS_RUN);
    wait_display_done();
    load_img_area_start();
    cs(0);
    wait_ready(); word_tx(0x0000);
    wait_ready();
    for (int g = 0; g < 16; g++) {
        memset(row, (uint8_t)((g << 4) | g), iPitch);
        int h = (g == 15) ? (EPD_HEIGHT - 15 * BAND_H) : BAND_H;
        spi_transaction_t t = {0};
        t.length = iPitch * 8; t.tx_buffer = row;
        for (int y = 0; y < h; y++) spi_device_polling_transmit(s_spi, &t);
    }
    cs(1);
    write_cmd(LD_IMG_END);
    uint16_t dargs[5] = { 0, 0, EPD_WIDTH, EPD_HEIGHT, MODE_GC16 };
    send_cmd_args(DPY_AREA, dargs, 5);
    wait_display_done();
    write_cmd(STANDBY);
    free(row);
    ESP_LOGI(TAG, "gray ramp done");
}

static void it8951_show_palette_sweep(void) { it8951_show_color_bars(); }

static void it8951_sleep(void)
{
    write_cmd(SLEEP);
    gpio_set_level(EPD_PIN_EN, 0);
    gpio_set_level(EPD_PIN_VCC_EN, 0);
}

/* ---------- exported vtable ---------- */

const epd_driver_t it8951_gray_driver = {
    .info = {
        .name      = "IT8951 grayscale 10.3\" (1872x1404, 4bpp)",
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
    },
    .port_init          = it8951_port_init,
    .init               = it8951_init,
    .clear              = it8951_clear,
    .display            = it8951_display,
    .show_color_bars    = it8951_show_color_bars,
    .show_palette_sweep = it8951_show_palette_sweep,
    .sleep              = it8951_sleep,
};

#endif /* PANEL_DRIVER_IT8951_GRAY */
