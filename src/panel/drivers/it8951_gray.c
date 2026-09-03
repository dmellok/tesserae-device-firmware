/*
 * Seeed reTerminal E1003 -- 10.3" ED103TC2 grayscale via IT8951, Family D.
 *
 * Ported from bitbank2 FastEPD's IT8951 path (initIT8951 +
 * setPanelSize(BBEP_DISPLAY_ED103TC2)). The IT8951 is a
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
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"          /* it8951_bench_wave timing */
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
#define MODE_INIT     0        /* drive every pixel to white, reference-free */
#define MODE_DU       1        /* fast 2-level "direct update" waveform */
#define MODE_GC16     2        /* 16-level grayscale waveform */

/* INIT waveform index. Mode 0 is INIT in every IT8951 waveform table we know
 * of; overridable per build in case a panel batch maps it elsewhere. */
#ifndef EPD_IT8951_INIT_MODE
#define EPD_IT8951_INIT_MODE MODE_INIT
#endif

static spi_device_handle_t s_spi;
static bool     s_port_inited = false;

/* A partial (area) pass ran since the last full-panel pass in this power
 * session. The controller derives every GC16 transition from the previous
 * image it holds, and after a wake that hard-reset it and loaded only an
 * echo rect, that reference no longer matches the glass outside the rect:
 * a plain GC16 full paint then under-drives every pixel the old frame had
 * dark and the old content shows through (dmellok/tesserae#274, seen on a
 * pushed frame painted in the touch-linger window after a DU tap echo).
 * A full paint that follows a partial first runs an INIT pass, which
 * drives the whole panel to white regardless of the reference, so the GC16
 * that follows starts from a reference that is true. */
static bool     s_partial_since_full;

static void fill_and_refresh(uint8_t color, int wave);
static uint32_t s_img_buf_addr;
/* Wedge-recovery deep-sleep budget (see it8951_init); cleared on success. */
static RTC_DATA_ATTR uint8_t s_recovery_sleeps;

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
    /* TRUE power-cycle. Holding CS/RST HIGH during the off-window kept the
     * controller parasitically alive through its input ESD diodes, so a
     * wedged SPI state machine (e.g. a reset landing mid-16-bit-word)
     * SURVIVED every "power cycle" -- only deep sleep, which isolates all
     * pins, cleared it (bench 2026-07-24: cold-boot HRDY wedge persisted
     * through 100 ms / 250 ms / 1 s rail-offs). Drive every line we own LOW
     * while the rails are down so the chip actually discharges. */
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_RST, 0);
    gpio_set_level(EPD_PIN_EN, 0);
    gpio_set_level(EPD_PIN_VCC_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(EPD_PIN_EN, 1);
    gpio_set_level(EPD_PIN_VCC_EN, 1);
    gpio_set_level(EPD_PIN_CS, 1);
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
    /* The SD card may share this bus (reTerminal boards) and have initialised
     * it already (with a compatible pin set and a larger transfer cap), so
     * tolerate INVALID_STATE like spectra6_spi_single does. */
    esp_err_t bus_err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(bus_err);
    }
    ESP_ERROR_CHECK(spi_bus_add_device(EPD_SPI_HOST, &dev, &s_spi));

    s_port_inited = true;
    return ESP_OK;
}

static void it8951_init(void)
{
    /* Init with verification + one long-off retry: a cold boot can leave the
     * controller wedged (HRDY stuck; bench 2026-07-24, likely a reset landing
     * mid-SPI-word). GET_DEV_INFO doubles as the health check -- a wedged
     * chip returns zeros/garbage instead of the panel geometry. */
    uint16_t info[20] = {0};

    /* Soft attach first: if the controller is already powered and idle
     * (HRDY high -- e.g. the SD layer booted it this wake, or a prior init),
     * DO NOT power-cycle it. Power-cycling a live controller is what wedges
     * it: our GPIO "off" never truly discharges the chip (SPI lines keep it
     * parasitically alive through the ESD diodes), so it comes back with a
     * corrupted state machine (bench 2026-07-24). Verify with GET_DEV_INFO;
     * only an unresponsive chip proceeds to the hard power-up ladder. */
    if (gpio_get_level(EPD_PIN_BUSY) == 1) {
        write_cmd(SYS_RUN);
        write_cmd(GET_DEV_INFO);
        read_ndata(info, 20);
        if (info[0] == EPD_WIDTH && info[1] == EPD_HEIGHT) {
            s_recovery_sleeps = 0;
            s_img_buf_addr = ((uint32_t)info[3] << 16) | info[2];
            write_cmd(VCOM); write_data(0x0001); write_data(EPD_VCOM_MV);
            write_reg(REG_I80CPCR, 0x0001);
            write_cmd(CMD_TEMP); write_data(0x0001); write_data(14);
            ESP_LOGI(TAG, "init complete (soft attach): dev %ux%u, img_buf=0x%08x",
                     info[0], info[1], (unsigned)s_img_buf_addr);
            return;
        }
        ESP_LOGW(TAG, "soft attach failed (dev %ux%u); hard power-up",
                 info[0], info[1]);
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "controller unresponsive; long power-off retry");
            gpio_set_level(EPD_PIN_CS, 0);    /* no ESD back-feed: all lines low */
            gpio_set_level(EPD_PIN_RST, 0);
            gpio_set_level(EPD_PIN_EN, 0);
            gpio_set_level(EPD_PIN_VCC_EN, 0);
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
        hw_reset_and_power();
        write_cmd(SYS_RUN);
        memset(info, 0, sizeof info);
        write_cmd(GET_DEV_INFO);
        read_ndata(info, 20);
        if (info[0] == EPD_WIDTH && info[1] == EPD_HEIGHT) {
            s_recovery_sleeps = 0;   /* healthy: reset the recovery budget */
            break;
        }
    }
    if (info[0] != EPD_WIDTH || info[1] != EPD_HEIGHT) {
        /* Last resort: the ONLY discharge proven to clear a wedged controller
         * is deep sleep -- the ESP isolates every pin, killing the parasitic
         * ESD-diode feed that keeps the chip alive through our GPIO "power
         * cycles" (SPI-owned SCLK/MOSI and the SD MISO pull-up still back-
         * feed even with CS/RST/rails low). Sleep 1 s and retry on the clean
         * wake path; the RTC counter stops a dead panel from boot-looping. */
        if (s_recovery_sleeps < 2) {
            s_recovery_sleeps++;
            ESP_LOGW(TAG, "controller wedged (dev %ux%u); 1 s isolation deep-sleep "
                     "(recovery %u/2)", info[0], info[1], s_recovery_sleeps);
            gpio_set_level(EPD_PIN_CS, 0);
            gpio_set_level(EPD_PIN_RST, 0);
            gpio_set_level(EPD_PIN_EN, 0);
            gpio_set_level(EPD_PIN_VCC_EN, 0);
            esp_deep_sleep(1000000);
            /* not reached */
        }
        ESP_LOGE(TAG, "controller failed init verify (dev %ux%u); paints may fail",
                 info[0], info[1]);
    }
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

static void load_img_area_start_rect(int cx, int cy, int cw, int ch)
{
    /* target address (LISAR) */
    write_reg(REG_LISAR + 2, (uint16_t)((s_img_buf_addr >> 16) & 0xFFFF));
    write_reg(REG_LISAR,     (uint16_t)(s_img_buf_addr & 0xFFFF));

    /* ensure 1bpp mode is off (we use 4bpp) */
    write_reg(REG_UP1SR + 2, (uint16_t)(read_reg(REG_UP1SR + 2) & ~(1 << 2)));

    /* info word: MIRROR_X -> big-endian; 4bpp; rotate 0. Then x,y,w,h in
     * CONTROLLER coordinates (post software mirror). */
    uint16_t args[5] = {
        (uint16_t)((ENDIAN_B << 8) | (PIXFMT_4BPP << 4) | 0),
        (uint16_t)cx, (uint16_t)cy, (uint16_t)cw, (uint16_t)ch,
    };
    send_cmd_args(LD_IMG_AREA, args, 5);
}

static void load_img_area_start(void)
{
    load_img_area_start_rect(0, 0, EPD_WIDTH, EPD_HEIGHT);
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
    s_partial_since_full = false;
    ESP_LOGI(TAG, "refresh done");
}

static void it8951_display(const uint8_t *image)
{
    if (s_partial_since_full) {
        ESP_LOGI(TAG, "full paint after a partial pass: INIT clear first");
        fill_and_refresh(0x0F, EPD_IT8951_INIT_MODE);
    }
    write_and_refresh(image);
}

/* Partial refresh (overlay feature): reload only the rect from the frame and
 * refresh it with DU (fast, mono-ish) or GC16 (hygiene). MEASURED on E1003
 * hardware (2026-07-27): DU costs ~1.07-1.17 s, not the ~300 ms this comment
 * used to claim. It is nearly INDEPENDENT of rect area -- 320x44 = 1065 ms,
 * 320x120 = 1067, 360x120 = 1170, 720x140 = 1169, so 7x the pixels costs 10%
 * more time -- and tracks rect WIDTH rather than area. The cost is the DU
 * waveform inside wait_display_done(), not the SPI load. Consequence for
 * callers: shrinking a refresh rect buys ghosting hygiene, NOT latency. The
 * lever that does move it is the waveform, which is why callers can ask for A2
 * (see it8951_display_partial_mode). The rect
 * arrives in FRAME coordinates; the ED103TC2's software MIRROR_X maps it to
 * controller x' = W - x - w, and within the window the bytes stream reversed
 * + nibble-swapped exactly like the full-frame path. x/w are widened to a
 * multiple of 4 px (whole bytes at 4bpp, and the IT8951 prefers 4-px
 * alignment for area operations). */
static void it8951_partial_wave(const uint8_t *fb, int x, int y, int w,
                                int h, int wave)
{
    /* Clamp to the panel, then widen to 4-px x-alignment. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_WIDTH)  w = EPD_WIDTH - x;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    int x0 = x & ~3;
    int x1 = (x + w + 3) & ~3;
    if (x1 > EPD_WIDTH) x1 = EPD_WIDTH;
    int aw = x1 - x0;
    int cx = EPD_WIDTH - x1;             /* mirrored window left edge */

    const int seg = aw / 2;              /* bytes per window row */
    uint8_t *scratch = heap_caps_malloc(seg, MALLOC_CAP_DMA);
    if (!scratch) { ESP_LOGE(TAG, "OOM partial scratch"); return; }

    write_cmd(SYS_RUN);
    wait_display_done();
    load_img_area_start_rect(cx, y, aw, h);

    cs(0);
    wait_ready(); word_tx(0x0000);       /* data preamble */
    wait_ready();
    const int iPitch = EPD_WIDTH / 2;
    for (int yy = y; yy < y + h; yy++) {
        const uint8_t *row = fb + (size_t)yy * iPitch + x0 / 2;
        for (int i = 0; i < seg; i++) {
            uint8_t b = row[seg - 1 - i];
            scratch[i] = (uint8_t)((b >> 4) | (b << 4));
        }
        spi_transaction_t t = {0};
        t.length = seg * 8; t.tx_buffer = scratch;
        spi_device_polling_transmit(s_spi, &t);
    }
    cs(1);
    write_cmd(LD_IMG_END);

    uint16_t dargs[5] = { (uint16_t)cx, (uint16_t)y, (uint16_t)aw, (uint16_t)h,
                          (uint16_t)wave };
    send_cmd_args(DPY_AREA, dargs, 5);
    wait_display_done();
    write_cmd(STANDBY);
    free(scratch);
    s_partial_since_full = true;
    ESP_LOGI(TAG, "partial mode %d (%d,%d %dx%d)", wave, x0, y, aw, h);
}

/* Waveform-mode numbers are indices into the waveform table in the PANEL's
 * flash, not fixed constants, so a wrong guess silently paints with some other
 * waveform. Overridable per board for exactly that reason.
 *
 * MEASURED on the E1003's ED103TC2 (selftest mode sweep, 320x44 rect,
 * 2026-07-27) -- note none of these match the canonical IT8951 datasheet
 * timings, which is why they were swept rather than assumed:
 *
 *     mode 1 (DU)    1330 ms      mode 5   641 ms
 *     mode 2 (GC16)  1564 ms      mode 6    220 ms   <- A2, selected
 *     mode 3          641 ms      mode 7   642 ms
 *     mode 4          220 ms
 *
 * Modes 4 and 6 tie at 220 ms; 6 is the conventional A2 index, so it wins on
 * convention alone -- if A2 ever renders wrong on a panel batch, try 4 next.
 *
 * DO NOT pick a waveform from this table on timing alone. A2 (6) was shipped
 * for touch-v3 feedback on exactly that reasoning and REVERTED the same day:
 * at 220 ms it is 5x faster than DU, but it GHOSTS TOO HEAVILY on this panel
 * to be usable, judged on real glass. Speed here is bought with fidelity, and
 * the table cannot show you that half. Modes 3/5/7 (~641 ms, grayscale rather
 * than 2-level) are the untested middle ground. */
#ifndef EPD_IT8951_A2_MODE
#define EPD_IT8951_A2_MODE 6
#endif

/* The fast GRAYSCALE partial -- the interactive default. Modes 3, 5 and 7 were
 * A/B'd on hardware (2026-07-27): all three ran 435-436 ms on a 320x44 rect
 * versus DU's 960 ms, all rendered the switch's mid-grey ON track correctly
 * with the thumb visible, and none ghosted objectionably. They are empirically
 * interchangeable here, so 5 is a tiebreak, not a finding -- if it ever
 * misbehaves on a panel batch, 3 and 7 are verified drop-in equals. */
#ifndef EPD_IT8951_GRAY_MODE
#define EPD_IT8951_GRAY_MODE 5
#endif

static void it8951_display_partial(const uint8_t *fb, int x, int y, int w,
                                   int h, bool fast)
{
    /* Unchanged semantics for the existing overlay/proto2 callers. */
    it8951_partial_wave(fb, x, y, w, h, fast ? MODE_DU : MODE_GC16);
}

static void it8951_display_partial_mode(const uint8_t *fb, int x, int y, int w,
                                        int h, epd_refresh_t mode)
{
    int wave = MODE_DU;
    if (mode == EPD_RF_A2)        wave = EPD_IT8951_A2_MODE;
    else if (mode == EPD_RF_GRAY) wave = EPD_IT8951_GRAY_MODE;
    else if (mode == EPD_RF_GC16) wave = MODE_GC16;
    it8951_partial_wave(fb, x, y, w, h, wave);
}

/* Diagnostic: time one raw waveform index on a rect (selftest mode sweep).
 * Returns the elapsed milliseconds. Not part of the vtable. */
int it8951_bench_wave(const uint8_t *fb, int x, int y, int w, int h, int wave)
{
    int64_t t0 = esp_timer_get_time();
    it8951_partial_wave(fb, x, y, w, h, wave);
    return (int)((esp_timer_get_time() - t0) / 1000);
}

/* Fill the whole panel with one gray level (nibble) using `wave`. */
static void fill_and_refresh(uint8_t color, int wave)
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
    uint16_t dargs[5] = { 0, 0, EPD_WIDTH, EPD_HEIGHT, (uint16_t)wave };
    send_cmd_args(DPY_AREA, dargs, 5);
    wait_display_done();
    write_cmd(STANDBY);
    free(row);
    s_partial_since_full = false;
    ESP_LOGI(TAG, "fill mode %d done", wave);
}

static void it8951_clear(uint8_t color)
{
    fill_and_refresh(color, MODE_GC16);
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
    s_partial_since_full = false;
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
        .grayscale = true,     /* 16 true gray levels: duotone is renderable */
    },
    .port_init          = it8951_port_init,
    .init               = it8951_init,
    .clear              = it8951_clear,
    .display            = it8951_display,
    .show_color_bars    = it8951_show_color_bars,
    .show_palette_sweep = it8951_show_palette_sweep,
    .sleep              = it8951_sleep,
    .display_partial      = it8951_display_partial,
    .display_partial_mode = it8951_display_partial_mode,
};

#endif /* PANEL_DRIVER_IT8951_GRAY */
