/*
 * Family F: raw parallel e-paper glass, 16-level grayscale (see the header).
 *
 * HOW THE PANEL IS DRIVEN
 *
 * The source (column) driver is a long shift register fed 8 bits at a time on
 * the data bus, one CL pulse per byte. Each pixel is TWO bits, so one byte
 * carries four pixels, MSB pair = leftmost:
 *
 *     0b00  neutral   (drive, but toward nothing -- also the discharge code)
 *     0b01  darken    (push this pixel toward black)
 *     0b10  lighten   (push this pixel toward white)
 *     0b11  skip      (leave the pixel floating)
 *
 * A row is therefore EPD_WIDTH/4 bytes, clocked out by the LCD/i80 peripheral
 * in one DMA burst, then latched into the driver outputs by a pulse on LE.
 * The gate (row) driver is a second shift register with no data bus at all:
 * SPV loads a single token and each CKV rising edge walks it down one row. So
 * a "pass" over the panel is: row_start() to seed the gate driver, then 540 x
 * (write a row, LE, CKV). Every pass drives the whole panel for one waveform
 * step; grey comes from repeating that with different codes.
 *
 * WHERE THE GREYS COME FROM
 *
 * EPD_PAR_GRAY_MATRIX is 16 rows (one per grey level) x N columns (one per
 * pass), each entry the 2-bit code above. Level 0 is eight darkens, level 15
 * eight lightens, and the levels between are the mixtures that land on evenly
 * spaced tones. The matrix assumes the panel STARTS FROM WHITE, which is why
 * every update runs the black/white clear cycle first.
 *
 * Pass count is derived from the matrix size, so retuning a panel's greys is a
 * table edit, not a code change. Two 256-entry lookups (built once at
 * port_init) turn a source byte -- two 4bpp pixels -- straight into its drive
 * codes for a given pass, so the per-row inner loop is table reads and ORs.
 *
 * TIMING
 *
 * At 40 MHz and 8 bits/clock a 240-byte row plus its 16 bytes of line padding
 * is ~6.4 us, so a pass over 540 rows is ~3.5 ms. A full update is 32 clear
 * passes + 8 grey passes ~= 150 ms. That is fast enough that the row loop runs
 * with interrupts on and no yield: FreeRTOS tick preemption inside a pass is
 * survivable here (a row is driven a fraction longer, not wrongly), and the
 * whole update is far shorter than a tick-free critical section should be.
 */
#include "app_config.h"          /* board.h -> PANEL_DRIVER_* selection */

#if defined(PANEL_DRIVER_PARALLEL_EPD_GRAY)

#include "drivers/parallel_epd_gray.h"

#include <stdlib.h>
#include <string.h>

/* Selftest sheet letters. font8x8_basic.h holds a definition, not a
 * declaration, and splash.c already owns the one copy of the array. */
extern char font8x8_basic[128][8];

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd_par";

/* ------------------------------------------------------------------ */
/* Board-supplied knobs, with the defaults the M5 4.7" glass wants.    */
/* ------------------------------------------------------------------ */

#ifndef EPD_PAR_PCLK_HZ
#define EPD_PAR_PCLK_HZ        (40 * 1000 * 1000)
#endif
#ifndef EPD_PAR_BUS_WIDTH
#define EPD_PAR_BUS_WIDTH      8
#endif
/* Extra bytes clocked out after each row. The source driver's shift register
 * is wider than the visible glass; without the trailing clocks the last real
 * pixels never reach their outputs. FastEPD calls this iLinePadding. */
#ifndef EPD_PAR_LINE_PADDING
#define EPD_PAR_LINE_PADDING   16
#endif
/* Settling time after a pass, before the next one seeds the gate driver. */
#ifndef EPD_PAR_PASS_SETTLE_US
#define EPD_PAR_PASS_SETTLE_US 230
#endif

/* Clear-cycle depth. The four-phase black/white/black/white cycle is FastEPD's
 * CLEAR_SLOW, its default for a full update: it costs ~120 ms and is what makes
 * the grey matrix's "starts from white" assumption true. */
#ifndef EPD_PAR_CLEAR_PASSES
#define EPD_PAR_CLEAR_PASSES   8
#endif

/* 2-bit drive codes, replicated across a byte (4 pixels). */
#define CODE_NEUTRAL  0x00
#define CODE_DARKEN   0x55
#define CODE_LIGHTEN  0xAA

#define ROW_BYTES     (EPD_WIDTH / 4)          /* 4 px per byte */
#define SRC_PITCH     (EPD_WIDTH / 2)          /* 4bpp source, 2 px per byte */
#define TX_BYTES      (ROW_BYTES + EPD_PAR_LINE_PADDING)

static const uint8_t s_matrix[] = EPD_PAR_GRAY_MATRIX;
#define GRAY_PASSES  ((int)(sizeof(s_matrix) / 16))

/* Optional candidate matrix under evaluation. When the board defines it, the
 * selftest paints the two matrices side by side, one per half of the scan
 * axis, so a single flash judges a retune. Normal display keeps using
 * EPD_PAR_GRAY_MATRIX until the candidate is promoted. */
#ifdef EPD_PAR_GRAY_MATRIX_B
static const uint8_t s_matrix_b[] = EPD_PAR_GRAY_MATRIX_B;
_Static_assert(sizeof(s_matrix_b) == sizeof(s_matrix),
               "candidate grey matrix must match the shipped matrix's size");
#endif

_Static_assert(EPD_WIDTH % 8 == 0, "parallel EPD needs a width divisible by 8");
_Static_assert(EPD_BUF_BYTES == (size_t)EPD_WIDTH * EPD_HEIGHT / 2,
               "parallel_epd_gray is a 4bpp driver; EPD_BUF_BYTES must be W*H/2");
_Static_assert(sizeof(s_matrix) % 16 == 0,
               "grey matrix must be 16 rows (one per level) x N passes");

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static esp_lcd_i80_bus_handle_t     s_bus;
static esp_lcd_panel_io_handle_t    s_io;
static bool                         s_port_inited;
static bool                         s_powered;

/* Two DMA line buffers, ping-ponged: the CPU builds row n+1 while the
 * peripheral is still shifting row n. Each is padded to TX_BYTES with zeros
 * (neutral), which is what the off-glass tail of the shift register should
 * see. */
static uint8_t *s_line[2];

/* pass-major 256-entry tables: source byte (two 4bpp pixels) -> the pair of
 * 2-bit drive codes for that pass, positioned in the high or low nibble of an
 * output byte. Built once from s_matrix. */
static uint8_t *s_code_hi;   /* GRAY_PASSES * 256 */
static uint8_t *s_code_lo;
#ifdef EPD_PAR_GRAY_MATRIX_B
static uint8_t *s_code_hi_b;
static uint8_t *s_code_lo_b;
#endif

static volatile bool s_dma_done = true;

static bool IRAM_ATTR on_tx_done(esp_lcd_panel_io_handle_t io,
                                 esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    (void)io; (void)ev; (void)ctx;
    s_dma_done = true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Gate-driver (row) control                                          */
/* ------------------------------------------------------------------ */

/* Seed the gate driver so the next latched row is row 0. The pulse widths are
 * FastEPD's PaperS3RowControl verbatim; they are panel timing, not style. */
static void row_start(void)
{
    gpio_set_level(EPD_PIN_CKV, 1);
    esp_rom_delay_us(7);
    gpio_set_level(EPD_PIN_SPV, 0);
    esp_rom_delay_us(10);
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_CKV, 1);
    esp_rom_delay_us(8);
    gpio_set_level(EPD_PIN_SPV, 1);
    esp_rom_delay_us(10);
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_CKV, 1);
    esp_rom_delay_us(18);
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_CKV, 1);
    esp_rom_delay_us(18);
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_CKV, 1);
}

/* Latch the row just shifted out (LE) and step the gate driver one row (CKV).
 * CKV goes back high in write_row(), just before the next burst. */
static void row_step(void)
{
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_LE, 1);
    gpio_set_level(EPD_PIN_LE, 0);
}

/* Shift one prepared line buffer into the source driver. `step` is false only
 * for the first row of a pass, which row_start() has already positioned.
 *
 * Note the one-row pipeline: LE latches the row shifted out LAST time, and the
 * CKV edge that follows selects it, so a row is driven while the next row's
 * data is on the bus. A pass therefore issues EPD_HEIGHT shifts but only
 * EPD_HEIGHT-1 latches, and the final row's data is never latched. That is
 * FastEPD's sequence as written, and the assumption is that row_start()'s four
 * leading CKV pulses put the loop over dummy lines the glass does not show. It
 * is left exactly as ported rather than "fixed" blind -- but if bring-up finds
 * the BOTTOM ROW stale or garbage while the rest of the frame is clean, this
 * is the paragraph to come back to: one extra row_step() after the loop is the
 * experiment. */
/* Spin until the peripheral is done with the previous line. A row is ~6 us, so
 * this normally returns immediately; the 100 ms cap exists because the
 * alternative on a mis-wired first bring-up is an unbounded spin that the task
 * watchdog turns into a silent reboot loop with nothing on the console. */
static void wait_dma(void)
{
    for (int i = 0; i < 100000 && !s_dma_done; i++) esp_rom_delay_us(1);
    if (!s_dma_done) {
        ESP_LOGE(TAG, "row DMA never completed; abandoning the wait");
        s_dma_done = true;
    }
}

static void write_row(const uint8_t *line, bool step)
{
    wait_dma();
    if (step) row_step();
    gpio_set_level(EPD_PIN_CKV, 1);
    s_dma_done = false;
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, -1, line, TX_BYTES);
    if (err != ESP_OK) {
        s_dma_done = true;
        ESP_LOGE(TAG, "row tx: %s", esp_err_to_name(err));
    }
}

static void pass_end(void)
{
    wait_dma();
    esp_rom_delay_us(EPD_PAR_PASS_SETTLE_US);
}

/* ------------------------------------------------------------------ */
/* Power                                                              */
/* ------------------------------------------------------------------ */

static void power(bool on)
{
    if (on == s_powered) return;
    if (on) {
        gpio_set_level(EPD_PIN_PWR, 1);      /* panel rail */
        esp_rom_delay_us(100);
        gpio_set_level(EPD_PIN_BST_EN, 1);   /* +/-15 V boost */
        esp_rom_delay_us(100);
        gpio_set_level(EPD_PIN_SPV, 1);
    } else {
        gpio_set_level(EPD_PIN_BST_EN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));        /* let the boost bleed down */
        gpio_set_level(EPD_PIN_PWR, 0);
        esp_rom_delay_us(100);
        gpio_set_level(EPD_PIN_SPV, 0);
    }
    s_powered = on;
}

/* ------------------------------------------------------------------ */
/* Passes                                                             */
/* ------------------------------------------------------------------ */

static void build_code_tables(const uint8_t *matrix, uint8_t *hi, uint8_t *lo)
{
    for (int pass = 0; pass < GRAY_PASSES; pass++) {
        for (int i = 0; i < 256; i++) {
            uint8_t left  = matrix[(i >> 4) * GRAY_PASSES + pass];
            uint8_t right = matrix[(i & 0x0F) * GRAY_PASSES + pass];
            uint8_t pair  = (uint8_t)((left << 2) | right);
            lo[pass * 256 + i] = pair;
            hi[pass * 256 + i] = (uint8_t)(pair << 4);
        }
    }
}

/* `count` passes of one uniform drive code over the whole panel. Both the
 * black/white clear cycle and the final discharge are this. */
static void uniform_passes(uint8_t code, int count)
{
    memset(s_line[0], code, ROW_BYTES);
    for (int k = 0; k < count; k++) {
        row_start();
        for (int y = 0; y < EPD_HEIGHT; y++) write_row(s_line[0], y != 0);
        pass_end();
    }
}

/* The clear cycle the grey matrix is calibrated against: black, white, black,
 * white, then one neutral pass to leave the drivers discharged. */
static void clear_cycle(void)
{
    uniform_passes(CODE_DARKEN,  EPD_PAR_CLEAR_PASSES);
    uniform_passes(CODE_LIGHTEN, EPD_PAR_CLEAR_PASSES);
    uniform_passes(CODE_DARKEN,  EPD_PAR_CLEAR_PASSES);
    uniform_passes(CODE_LIGHTEN, EPD_PAR_CLEAR_PASSES);
    uniform_passes(CODE_NEUTRAL, 1);
}

/* The grey waveform: GRAY_PASSES passes over the 4bpp frame. Each source byte
 * holds two pixels and each output byte four, so eight source bytes make four
 * output bytes -- unrolled by four to keep the pointer arithmetic honest. */
static inline void translate_row(uint8_t *d, const uint8_t *s,
                                 const uint8_t *hi, const uint8_t *lo)
{
    for (int n = 0; n < ROW_BYTES; n += 4) {
        d[n + 0] = (uint8_t)(hi[s[0]] | lo[s[1]]);
        d[n + 1] = (uint8_t)(hi[s[2]] | lo[s[3]]);
        d[n + 2] = (uint8_t)(hi[s[4]] | lo[s[5]]);
        d[n + 3] = (uint8_t)(hi[s[6]] | lo[s[7]]);
        s += 8;
    }
}

static void gray_passes(const uint8_t *fb)
{
    for (int pass = 0; pass < GRAY_PASSES; pass++) {
        const uint8_t *hi = s_code_hi + pass * 256;
        const uint8_t *lo = s_code_lo + pass * 256;
        row_start();
        int slot = 0;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            translate_row(s_line[slot], fb + (size_t)y * SRC_PITCH, hi, lo);
            write_row(s_line[slot], y != 0);
            slot ^= 1;
        }
        pass_end();
    }
}

/* As gray_passes(), but every pixel in a row shares one grey level, so the
 * output row is a single repeated byte. Used by the diagnostics, which would
 * otherwise need a whole 259200-byte scratch frame to say "16 bands". */
static void gray_passes_bands(const uint8_t *level_per_row)
{
    for (int pass = 0; pass < GRAY_PASSES; pass++) {
        const uint8_t *hi = s_code_hi + pass * 256;
        const uint8_t *lo = s_code_lo + pass * 256;
        row_start();
        int slot = 0;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            uint8_t g = (uint8_t)(level_per_row[y] & 0x0F);
            uint8_t src = (uint8_t)((g << 4) | g);
            memset(s_line[slot], (uint8_t)(hi[src] | lo[src]), ROW_BYTES);
            write_row(s_line[slot], y != 0);
            slot ^= 1;
        }
        pass_end();
    }
}

/* ------------------------------------------------------------------ */
/* Selftest sheet: scrambled, lettered ramp bars                       */
/* ------------------------------------------------------------------ */

/* 16 vertical bars, one per grey level, in bit-reversed level order so no two
 * numerically adjacent levels sit next to each other on the glass. Letters
 * A..P (screen position order) sit under each bar on a white strip, so a
 * human can report the ramp as a letter ordering, darkest to lightest. That
 * ordinal report is immune to the photo lighting gradients that bias
 * luminance measurement, and the scramble turns any residual gradient into
 * noise rather than bias. Top half of the sheet is the shipped matrix,
 * bottom half the candidate (when the board defines one). */
#define SHEET_BARS     16
#define SHEET_BAR_W    (EPD_WIDTH / SHEET_BARS)
#define SHEET_STRIP_H  44
#define SHEET_DIV_H    4
#define SHEET_GLYPH_S  4                        /* font8x8 scale: 32 px glyphs */

static const uint8_t k_sheet_perm[SHEET_BARS] =
    { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };

/* Set one 4bpp pixel in a source row (high nibble = left pixel of the pair). */
static void sheet_px(uint8_t *row, int x, uint8_t g)
{
    uint8_t *b = &row[x >> 1];
    if (x & 1) *b = (uint8_t)((*b & 0xF0) | g);
    else       *b = (uint8_t)((*b & 0x0F) | (uint8_t)(g << 4));
}

/* One frame of the sheet. bars_src is the (single) source row every bar row
 * shares; strip_src is SHEET_STRIP_H rows of letter strip, reused for both
 * strips; black_src is the divider row. */
static void gray_passes_sheet(const uint8_t *bars_src, const uint8_t *strip_src,
                              const uint8_t *black_src)
{
    const int ts0 = (EPD_HEIGHT - 2 * SHEET_STRIP_H - SHEET_DIV_H) / 2;
    const int ts1 = ts0 + SHEET_STRIP_H;        /* top strip end / divider   */
    const int dv1 = ts1 + SHEET_DIV_H;          /* divider end / bottom bars */
    const int bs0 = EPD_HEIGHT - SHEET_STRIP_H; /* bottom strip start        */

    for (int pass = 0; pass < GRAY_PASSES; pass++) {
        const uint8_t *hi_a = s_code_hi + pass * 256;
        const uint8_t *lo_a = s_code_lo + pass * 256;
        const uint8_t *hi_b = hi_a, *lo_b = lo_a;
#ifdef EPD_PAR_GRAY_MATRIX_B
        if (s_code_hi_b) {
            hi_b = s_code_hi_b + pass * 256;
            lo_b = s_code_lo_b + pass * 256;
        }
#endif
        row_start();
        int slot = 0;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            const uint8_t *src;
            if      (y < ts0) src = bars_src;
            else if (y < ts1) src = strip_src + (size_t)(y - ts0) * SRC_PITCH;
            else if (y < dv1) src = black_src;
            else if (y < bs0) src = bars_src;
            else              src = strip_src + (size_t)(y - bs0) * SRC_PITCH;
            translate_row(s_line[slot], src,
                          y < dv1 ? hi_a : hi_b, y < dv1 ? lo_a : lo_b);
            write_row(s_line[slot], y != 0);
            slot ^= 1;
        }
        pass_end();
    }
}

/* ------------------------------------------------------------------ */
/* Driver entry points                                                */
/* ------------------------------------------------------------------ */

static esp_err_t par_port_init(void)
{
    if (s_port_inited) return ESP_OK;

    /* Everything except the data bus, CL and SPH -- those belong to the LCD
     * peripheral from esp_lcd_new_panel_io_i80() on. */
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << EPD_PIN_PWR) | (1ULL << EPD_PIN_BST_EN) |
                        (1ULL << EPD_PIN_SPV) | (1ULL << EPD_PIN_CKV) |
                        (1ULL << EPD_PIN_LE),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(EPD_PIN_PWR, 0);
    gpio_set_level(EPD_PIN_BST_EN, 0);
    gpio_set_level(EPD_PIN_SPV, 0);
    gpio_set_level(EPD_PIN_CKV, 0);
    gpio_set_level(EPD_PIN_LE, 0);
    s_powered = false;

    s_line[0] = heap_caps_aligned_alloc(64, TX_BYTES, MALLOC_CAP_DMA);
    s_line[1] = heap_caps_aligned_alloc(64, TX_BYTES, MALLOC_CAP_DMA);
    s_code_hi = malloc((size_t)GRAY_PASSES * 256);
    s_code_lo = malloc((size_t)GRAY_PASSES * 256);
    if (!s_line[0] || !s_line[1] || !s_code_hi || !s_code_lo) {
        ESP_LOGE(TAG, "OOM (line buffers / code tables)");
        free(s_line[0]); free(s_line[1]); free(s_code_hi); free(s_code_lo);
        s_line[0] = s_line[1] = s_code_hi = s_code_lo = NULL;
        return ESP_ERR_NO_MEM;
    }
    memset(s_line[0], 0, TX_BYTES);
    memset(s_line[1], 0, TX_BYTES);

    /* source byte i = pixels (i >> 4, i & 0xF), left pixel first. In an output
     * byte the MSB pair is the leftmost pixel, so the left source pixel's code
     * sits two bits above the right one. */
    build_code_tables(s_matrix, s_code_hi, s_code_lo);
#ifdef EPD_PAR_GRAY_MATRIX_B
    s_code_hi_b = malloc((size_t)GRAY_PASSES * 256);
    s_code_lo_b = malloc((size_t)GRAY_PASSES * 256);
    if (!s_code_hi_b || !s_code_lo_b) {
        ESP_LOGE(TAG, "OOM (candidate-matrix code tables)");
        free(s_code_hi_b); free(s_code_lo_b);
        s_code_hi_b = s_code_lo_b = NULL;   /* selftest falls back to A only */
    } else {
        build_code_tables(s_matrix_b, s_code_hi_b, s_code_lo_b);
    }
#endif

    /* The i80 driver insists on a D/C pin it will never need here, so the
     * board donates an unused GPIO to absorb it (FastEPD does the same). */
    const esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num = EPD_PIN_DC_DUMMY,
        .wr_gpio_num = EPD_PIN_CL,
        .clk_src = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums = EPD_PAR_DATA_PINS,
        .bus_width = EPD_PAR_BUS_WIDTH,
        .max_transfer_bytes = TX_BYTES,
        .dma_burst_size = 32,
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &s_bus));

    const esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = EPD_PIN_SPH,      /* SPH is the row's start/enable */
        .pclk_hz = EPD_PAR_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_tx_done,
        .lcd_cmd_bits = 0,
        .lcd_param_bits = 0,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0,
                       .dc_dummy_level = 0, .dc_data_level = 1 },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(s_bus, &io_cfg, &s_io));

    s_dma_done = true;
    s_port_inited = true;
    ESP_LOGI(TAG, "%dx%d 4bpp, %d grey passes, %d MHz bus",
             EPD_WIDTH, EPD_HEIGHT, GRAY_PASSES, EPD_PAR_PCLK_HZ / 1000000);
    return ESP_OK;
}

/* No init sequence to send -- there is no controller to send one to. Raising
 * the rails IS the init; sleep() drops them again. */
static void par_init(void)
{
    power(true);
}

static void par_display(const uint8_t *image)
{
    power(true);
    clear_cycle();
    gray_passes(image);
    uniform_passes(CODE_NEUTRAL, 1);   /* leave the drivers discharged */
    ESP_LOGI(TAG, "refresh done");
}

/* Fill the panel with one grey level. Runs the same clear + matrix path as a
 * frame so the level lands where the ramp says it should, rather than being
 * "however white the clear left it". */
static void par_clear(uint8_t color)
{
    uint8_t g = (uint8_t)(color & 0x0F);
    uint8_t rows[EPD_HEIGHT];
    memset(rows, g, sizeof rows);

    power(true);
    clear_cycle();
    gray_passes_bands(rows);
    uniform_passes(CODE_NEUTRAL, 1);
    ESP_LOGI(TAG, "clear to level %u done", g);
}

/* Diagnostic: the scrambled, lettered ramp sheet (see gray_passes_sheet).
 * If the matrix is tuned for this glass, sorting the bars darkest to
 * lightest reads out the letter sequence logged below; any deviation names
 * the exact levels to retune, with no photometry involved. */
static void par_show_color_bars(void)
{
    uint8_t bars_src[SRC_PITCH];
    uint8_t black_src[SRC_PITCH];
    uint8_t *strip = malloc((size_t)SHEET_STRIP_H * SRC_PITCH);

    if (!strip) {                       /* fall back to the plain ramp */
        uint8_t rows[EPD_HEIGHT];
        const int band = EPD_HEIGHT / 16;
        for (int y = 0; y < EPD_HEIGHT; y++) {
            int g = y / band;
            rows[y] = (uint8_t)(g > 15 ? 15 : g);
        }
        power(true);
        clear_cycle();
        gray_passes_bands(rows);
        uniform_passes(CODE_NEUTRAL, 1);
        ESP_LOGI(TAG, "grey ramp done (no RAM for the lettered sheet)");
        return;
    }

    for (int x = 0; x < EPD_WIDTH; x++)
        sheet_px(bars_src, x, k_sheet_perm[x / SHEET_BAR_W]);
    memset(black_src, 0x00, sizeof black_src);          /* level 0 divider */
    memset(strip, 0xFF, (size_t)SHEET_STRIP_H * SRC_PITCH);  /* white strip */

    const int gx0 = (SHEET_BAR_W - 8 * SHEET_GLYPH_S) / 2;
    const int gy0 = (SHEET_STRIP_H - 8 * SHEET_GLYPH_S) / 2;
    for (int pos = 0; pos < SHEET_BARS; pos++) {
        const char *g = font8x8_basic['A' + pos];       /* LSB = leftmost */
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                if (!((g[row] >> col) & 1)) continue;
                for (int dy = 0; dy < SHEET_GLYPH_S; dy++) {
                    uint8_t *r = strip +
                        (size_t)(gy0 + row * SHEET_GLYPH_S + dy) * SRC_PITCH;
                    for (int dx = 0; dx < SHEET_GLYPH_S; dx++)
                        sheet_px(r, pos * SHEET_BAR_W + gx0 +
                                    col * SHEET_GLYPH_S + dx, 0);
                }
            }
    }

    power(true);
    clear_cycle();
    gray_passes_sheet(bars_src, strip, black_src);
    uniform_passes(CODE_NEUTRAL, 1);
    free(strip);

    ESP_LOGI(TAG, "lettered ramp sheet done: bars A..P left to right = levels "
                  "0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15");
    ESP_LOGI(TAG, "a perfect ramp sorts darkest->lightest as "
                  "A I E M C K G O B J F N D L H P"
#ifdef EPD_PAR_GRAY_MATRIX_B
                  " (top half = shipped matrix, bottom half = candidate)"
#endif
                  );
}

static void par_show_palette_sweep(void) { par_show_color_bars(); }

static void par_sleep(void)
{
    power(false);
}

/* ---------- exported vtable ---------- */

const epd_driver_t parallel_epd_gray_driver = {
    .info = {
        .name      = EPD_PAR_PANEL_NAME,
        .width     = EPD_WIDTH,
        .height    = EPD_HEIGHT,
        .bpp       = 4,
        .buf_bytes = EPD_BUF_BYTES,
        .grayscale = true,     /* 16 true grey levels: duotone is renderable */
    },
    .port_init          = par_port_init,
    .init               = par_init,
    .clear              = par_clear,
    .display            = par_display,
    .show_color_bars    = par_show_color_bars,
    .show_palette_sweep = par_show_palette_sweep,
    .sleep              = par_sleep,
    /* No partial refresh yet. This family CAN do it -- a rect-clipped pass
     * with the rest of the row masked to skip codes -- but it needs the
     * previous frame kept in PSRAM to diff against, and it is the one part
     * that cannot be judged from a photo of a static frame. Left NULL, so
     * overlay/touch-v3 stay unadvertised rather than half-working. */
};

#endif /* PANEL_DRIVER_PARALLEL_EPD_GRAY */
