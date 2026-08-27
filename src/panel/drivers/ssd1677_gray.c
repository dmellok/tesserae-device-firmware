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

#include <stdlib.h>
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
#define SSD_CTRL1       0x21   /* plane bypass */
#define SSD_UPD_CTRL    0x22
#define SSD_TRIGGER     0x20
#define SSD_SLEEP       0x10

/* Update-control byte for a full 4-gray refresh (Seeed's EPD_UPDATE_GRAY).
 * The mono path uses 0xF7; 0xD7 is the grayscale variant, which drives the
 * OTP waveform table rather than the black/white one. */
#define SSD_UPD_GRAY    0xd7

/* Update-control byte for a full MONO refresh. Differs from the grayscale one
 * by the load-temperature bit: the gray path must NOT reload temperature,
 * because the fixed value written to 0x1A is what selects the 4-gray waveform.
 * Used only for the conditioning clear below, which wants a proper clearing
 * waveform rather than the gray one. */
#define SSD_UPD_MONO    0xf7

/* See ssd1677_init() for why this defaults off. */
#ifndef SSD1677_CONDITION_CLEAR
#define SSD1677_CONDITION_CLEAR 0
#endif

/* Transpose when the glass is mounted at 90 degrees, i.e. when the frame and
 * the controller's scan disagree. Derived rather than asserted: the Sticky is
 * 480x800 frame over an 800x480 scan, the X4 is 800x480 both ways. */
#ifndef EPD_SSD1677_TRANSPOSE
#  if (EPD_WIDTH == EPD_PANEL_SCAN_W) && (EPD_HEIGHT == EPD_PANEL_SCAN_H)
#    define EPD_SSD1677_TRANSPOSE 0
#  else
#    define EPD_SSD1677_TRANSPOSE 1
#  endif
#endif
#ifndef EPD_MIRROR_Y
#define EPD_MIRROR_Y 0
#endif

#ifndef EPD_SSD1677_TRANSPOSE
#  if (EPD_WIDTH == EPD_PANEL_SCAN_W) && (EPD_HEIGHT == EPD_PANEL_SCAN_H)
#    define EPD_SSD1677_TRANSPOSE 0
#  else
#    define EPD_SSD1677_TRANSPOSE 1
#  endif
#endif

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

#ifdef EPD_MONO
/* ---------- 1bpp mono encoding ----------
 *
 * The frame is 8 px/byte MSB-first, bit 1 = WHITE (the mono_spi convention),
 * and the mono waveform reads the BW RAM the same way -- so the plane is a
 * straight copy. Note this does NOT match GRAY_P1_BIT above, which inverts:
 * same 0x24 RAM, but the 4-gray LUT indexes it the other way round. Observed
 * on an X4; inverting here paints white on black. */
#ifndef EPD_MONO_INVERT
#define EPD_MONO_INVERT 0
#endif
#if EPD_MONO_INVERT
#  define MONO_PLANE_BYTE(b) ((uint8_t)~(uint8_t)(b))
#else
#  define MONO_PLANE_BYTE(b) ((uint8_t)(b))
#endif
#define MONO_SOLID(white) MONO_PLANE_BYTE((white) ? 0xFF : 0x00)
#endif /* EPD_MONO */

/* Hold the SPI bus for a whole frame.
 *
 * CS is driven by hand here, and a full plane is 48000 bytes sent as a dozen
 * chunked transactions with CS held LOW throughout. On this board the microSD
 * shares SCLK and MOSI with the panel, so any SD transaction that interleaves
 * between our chunks goes out while the panel is still selected -- and the panel
 * takes it as image data. The result is snow, and it appears ONLY once something
 * else is using the bus, which is why the no-networking selftest rendered
 * perfectly while real frames did not.
 *
 * spi_device_acquire_bus() blocks other devices for the duration, which is the
 * guarantee a hand-driven CS needs and does not otherwise have. */
static void bus_hold(void)    { spi_device_acquire_bus(s_spi, portMAX_DELAY); }
static void bus_release(void) { spi_device_release_bus(s_spi); }

/* Map a controller scan row to the frame row it reads from. */
static inline int mirror_y(int cy) { return EPD_MIRROR_Y ? (EPD_PANEL_SCAN_H - 1 - cy) : cy; }

/* One 2bpp pixel out of the frame, in FRAME coordinates. */
static inline uint8_t frame_px(const uint8_t *image, int fx, int fy)
{
    const uint8_t b = image[(size_t)fy * (EPD_WIDTH / 4) + (fx >> 2)];
    return (uint8_t)((b >> ((3 - (fx & 3)) * 2)) & 0x3);
}

/* Stream one 1bpp plane onto the controller's scan.
 *
 * The controller always scans landscape (SCAN_W x SCAN_H) whatever the glass is
 * mounted like. On a panel mounted at 90 degrees a controller row is a frame
 * COLUMN: while emitting controller row cy we walk cx and read frame pixel
 * (fx = cy, fy = cx).
 *
 * That direction was fixed on Sticky hardware rather than guessed. A pattern
 * drawn as horizontal bands in controller space appeared as vertical columns
 * reading black on the LEFT, which pins fx to cy without an inversion; had it
 * been mirrored the black band would have come out on the right.
 *
 * Transposing walks the frame with a SCAN_H/4-byte stride instead of
 * sequentially. That is the cost of doing the rotation here, and it is the
 * right place for it: the alternative is the server rendering sideways content
 * for one panel. A panel whose glass matches the scan (EPD_SSD1677_TRANSPOSE 0,
 * e.g. the X4) reads the frame straight through, which is also the faster
 * path. */
static void gray_send_plane(uint8_t plane_cmd, const uint8_t *image, int plane)
{
    uint8_t row[EPD_PANEL_SCAN_W / 8];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(plane_cmd);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        const int my = mirror_y(cy);
        for (int b = 0; b < EPD_PANEL_SCAN_W / 8; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++) {
                const int cx = b * 8 + k;
#if EPD_SSD1677_TRANSPOSE
                uint8_t g   = frame_px(image, my, cx);
#else
                uint8_t g   = frame_px(image, cx, my);
#endif
                uint8_t bit = (plane == 1) ? GRAY_P1_BIT(g) : GRAY_P2_BIT(g);
                o = (uint8_t)((o << 1) | (bit & 1));
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

#ifdef EPD_MONO
/* Stream the 1bpp frame into the BW plane (0x24). Untransposed, a frame row is
 * a controller row and the packing already matches, so it is a row copy rather
 * than the gray path's per-pixel gather. */
static void mono_send_plane(const uint8_t *image)
{
    uint8_t row[EPD_PANEL_SCAN_W / 8];

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        const int my = mirror_y(cy);
#if EPD_SSD1677_TRANSPOSE
        for (int b = 0; b < EPD_PANEL_SCAN_W / 8; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++) {
                /* Controller row cy is a frame COLUMN: fx = cy, fy = cx. */
                int fx = my, fy = b * 8 + k;
                uint8_t bit = (image[(size_t)fy * (EPD_WIDTH / 8) + (fx >> 3)]
                               >> (7 - (fx & 7))) & 1;
                o = (uint8_t)((o << 1) | bit);
            }
            row[b] = MONO_PLANE_BYTE(o);
        }
#else
        const uint8_t *src = image + (size_t)my * (EPD_WIDTH / 8);
        const int stride = EPD_PANEL_SCAN_W / 8;
        for (int b = 0; b < stride; b++) row[b] = MONO_PLANE_BYTE(src[b]);
#endif
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Fill the BW plane with one level. */
static void mono_send_solid(uint8_t fill)
{
    uint8_t row[EPD_PANEL_SCAN_W / 8];
    memset(row, fill, sizeof row);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Ignore 0x26, the differential partner of 0x24: left in play, pixels matching
 * the previous paint may not re-drive and the frame comes out patchy. A full
 * refresh wants to depend on 0x24 alone. */
static void mono_bypass_second_plane(void)
{
    static const uint8_t BYPASS[] = {0x40};
    cmd_data(SSD_CTRL1, BYPASS, sizeof BYPASS);
}
#endif /* EPD_MONO */

/* Emit `rows` solid plane rows (caller has sent the plane command, CS low). */
static void gray_send_solid_rows(uint8_t fill, int rows)
{
    uint8_t row[EPD_PANEL_SCAN_W / 8];
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
    const uint8_t drv[] = {(uint8_t)((EPD_PANEL_SCAN_H - 1) & 0xff),
                           (uint8_t)((EPD_PANEL_SCAN_H - 1) >> 8), 0x02};
    cmd_data(SSD_DRV_OUTPUT, drv, sizeof drv);

    static const uint8_t ENTRY[] = {0x03};       /* X inc, Y inc */
    cmd_data(SSD_DATA_ENTRY, ENTRY, sizeof ENTRY);

    const uint8_t xwin[] = {0x00, 0x00,
                            (uint8_t)((EPD_PANEL_SCAN_W - 1) & 0xff),
                            (uint8_t)((EPD_PANEL_SCAN_W - 1) >> 8)};
    cmd_data(SSD_RAM_X, xwin, sizeof xwin);
    const uint8_t ywin[] = {0x00, 0x00,
                            (uint8_t)((EPD_PANEL_SCAN_H - 1) & 0xff),
                            (uint8_t)((EPD_PANEL_SCAN_H - 1) >> 8)};
    cmd_data(SSD_RAM_Y, ywin, sizeof ywin);

    static const uint8_t TSENS[] = {0x80};       /* internal temperature sensor */
    cmd_data(SSD_TEMP_SENSOR, TSENS, sizeof TSENS);

    static const uint8_t ZERO2[] = {0x00, 0x00};
    cmd_data(SSD_RAM_X_ADDR, ZERO2, sizeof ZERO2);
    cmd_data(SSD_RAM_Y_ADDR, ZERO2, sizeof ZERO2);
    wait_idle();

    /* ORDER MATTERS, and getting it wrong looks like static rather than like a
     * mistake. The conditioning clear runs a MONO update (0xF7), and that byte
     * includes the load-temperature step -- so it reloads the measured
     * temperature and discards any waveform selection made before it. Selecting
     * 4-gray first and clearing second therefore leaves the panel driving two
     * grayscale planes through a mono LUT, which paints noise.
     *
     * Clear first, select second. Seeed's examples do exactly this, in this
     * order: fillScreen(WHITE) + update(), and only then initGrayMode(). */
    /* A white conditioning pass before the grayscale paint, off by default.
     *
     * Seeed's grayscale examples do this, and e-paper genuinely can ghost when a
     * gray waveform paints over unrelated content. It was added here chasing a
     * "ghosted, then snow" symptom that turned out to be an unpowered SD card
     * corrupting the shared SPI bus (see SD_PIN_EN in the board header), not a
     * waveform problem at all -- so it is kept, documented, and NOT paid for
     * until a real ghosting case is observed on this panel.
     *
     * Enabling it costs a second full refresh on every wake, roughly doubling
     * panel time. If ghosting does show up across successive differing frames,
     * this is the switch. Note the ORDER below matters: the clear runs a mono
     * update, which reloads temperature and would discard a 4-gray waveform
     * selection made before it. */
#if SSD1677_CONDITION_CLEAR
    panel_condition_white();
#endif

    /* Fixed temperature: selects the 4-gray OTP waveform rather than
     * compensating for ambient, exactly like TSSET on the UC8179 gen2 path. It
     * survives because the gray refresh (0xD7) omits the load-temperature step
     * that the mono clear above performs. Must not be swapped for a live sensor
     * reading, and must not be issued before the clear. */
    static const uint8_t TWRITE[] = {0x67, 0x00};
    cmd_data(SSD_TEMP_WRITE, TWRITE, sizeof TWRITE);
    cmd_data(SSD_RAM_X_ADDR, ZERO2, sizeof ZERO2);
    cmd_data(SSD_RAM_Y_ADDR, ZERO2, sizeof ZERO2);
    wait_idle();

    ESP_LOGI(TAG, "init complete (SSD1677 4-gray, scan %dx%d, frame %dx%d)",
             EPD_PANEL_SCAN_W, EPD_PANEL_SCAN_H, EPD_WIDTH, EPD_HEIGHT);
}

/* Refresh an already-loaded frame with the given update-control byte. */
static void trigger_refresh_mode(uint8_t mode, const char *what)
{
    const uint8_t upd[] = {mode};
    int64_t t0 = esp_timer_get_time();
    cmd_data(SSD_UPD_CTRL, upd, sizeof upd);
    cmd_data(SSD_TRIGGER, NULL, 0);
    wait_idle();
    ESP_LOGI(TAG, "%s done (%lld ms)", what,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void trigger_refresh(void) { trigger_refresh_mode(SSD_UPD_GRAY, "refresh"); }

/* Drive the whole panel white with the MONO waveform, leaving it conditioned
 * for a grayscale paint.
 *
 * The 4-gray waveform is not a clearing waveform. It omits the load-temperature
 * step so the fixed 0x1A selection stands, and it assumes it is starting from
 * white -- so painting it straight over existing content leaves the old image
 * ghosting through and the levels short of fully developed. That is exactly how
 * this looked on first bring-up: the selftest stripes still visible under the
 * next frame, and everything fuzzy.
 *
 * Seeed's own grayscale examples do this and it is easy to miss, because it
 * lives in their example flow rather than their driver:
 *
 *     epaper.fillScreen(TFT_WHITE);
 *     epaper.update();                    // mono clear, THEN
 *     epaper.initGrayMode(GRAY_LEVEL4);
 *
 * Costs one extra refresh per wake. Correctness first; if that proves too slow
 * on battery, the place to optimise is skipping it when the panel is known to
 * already be white, not dropping it. */
#if SSD1677_CONDITION_CLEAR
static void panel_condition_white(void)
{
    bus_hold();
    /* White is (0,0) in both planes -- see the encoding table above. */
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    gray_send_solid_rows(GRAY_P1_BIT(3) ? 0xFF : 0x00, EPD_PANEL_SCAN_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    gray_send_solid_rows(GRAY_P2_BIT(3) ? 0xFF : 0x00, EPD_PANEL_SCAN_H);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh_mode(SSD_UPD_MONO, "conditioning clear");
    bus_release();
}
#endif /* SSD1677_CONDITION_CLEAR */

static void ssd1677_display(const uint8_t *image)
{
    bus_hold();
#ifdef EPD_MONO
    mono_bypass_second_plane();
    mono_send_plane(image);
    trigger_refresh_mode(SSD_UPD_MONO, "refresh");
#else
    gray_send_plane(SSD_DTM1, image, 1);
    gray_send_plane(SSD_DTM2, image, 2);
    trigger_refresh();
#endif
    bus_release();
}

static void ssd1677_clear(uint8_t color)
{
    bus_hold();
#ifdef EPD_MONO
    mono_bypass_second_plane();
    mono_send_solid(MONO_SOLID(color & 1));
    trigger_refresh_mode(SSD_UPD_MONO, "clear");
    bus_release();
    return;
#else
    uint8_t g  = (uint8_t)(color & 0x3);
    uint8_t p1 = GRAY_P1_BIT(g) ? 0xFF : 0x00;
    uint8_t p2 = GRAY_P2_BIT(g) ? 0xFF : 0x00;

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    gray_send_solid_rows(p1, EPD_PANEL_SCAN_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    gray_send_solid_rows(p2, EPD_PANEL_SCAN_H);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
    bus_release();
#endif /* EPD_MONO */
}

#ifdef EPD_MONO
/* Bring-up, mono: four alternating bands starting BLACK, a white wedge in the
 * frame's top-left, and a 1px stripe block. Bands prove geometry, the wedge
 * proves orientation, the stripes prove packing. Starting white means
 * EPD_MONO_INVERT is wrong for this glass. */
static void ssd1677_show_color_bars(void)
{
    uint8_t *frame = malloc(EPD_BUF_BYTES);
    if (!frame) return;
    const int BAND_H = EPD_HEIGHT / 4;
    const int stride = EPD_WIDTH / 8;      /* 1bpp: 8 px/byte */

    for (int fy = 0; fy < EPD_HEIGHT; fy++) {
        int band = fy / BAND_H;
        if (band > 3) band = 3;
        /* bit 1 = white, so an even band is black (0x00). */
        memset(frame + (size_t)fy * stride, (band & 1) ? 0xff : 0x00, stride);
    }

    /* Bottom half: fine detail, which bands cannot test -- a uniform block hides
     * a wrong stride or reversed bit order (the Sticky's bands passed while real
     * content came out as snow). Left: vertical stripes, bit order within a
     * byte. Right: horizontal stripes, row stride. */
    for (int fy = EPD_HEIGHT / 2; fy < EPD_HEIGHT; fy++) {
        for (int fx = 0; fx < EPD_WIDTH; fx++) {
            int black = (fx < EPD_WIDTH / 2) ? (fx & 1) : (fy & 1);
            size_t i = (size_t)fy * stride + (fx >> 3);
            uint8_t m = (uint8_t)(1u << (7 - (fx & 7)));
            if (black) frame[i] &= (uint8_t)~m;
            else       frame[i] |= m;
        }
    }

    /* Orientation wedge: WHITE, so it shows against black band 0. */
    const int WEDGE = (EPD_WIDTH < EPD_HEIGHT ? EPD_WIDTH : EPD_HEIGHT) / 10;
    for (int fy = 0; fy < WEDGE; fy++)
        memset(frame + (size_t)fy * stride, 0xff, WEDGE / 8);

    ssd1677_display(frame);
    free(frame);
}

/* Transport check: alternating pixels across the whole frame. */
static void ssd1677_show_palette_sweep(void)
{
    uint8_t *frame = malloc(EPD_BUF_BYTES);
    if (!frame) return;
    memset(frame, 0xAA, EPD_BUF_BYTES);   /* 1px vertical stripes */
    ssd1677_display(frame);
    free(frame);
}

#else /* !EPD_MONO */

/* Bring-up: four bands in FRAME coordinates, black at the top through white at
 * the bottom, plus a black wedge in the top-left corner.
 *
 * Built as a real frame and pushed through ssd1677_display() rather than
 * streamed straight at the controller, so the selftest exercises the SAME
 * rotation the production path uses. A selftest that bypasses the transform
 * cannot tell you the transform is right, which is exactly the gap the first
 * bring-up hit: bands drawn in controller space came out as columns and could
 * not say which way round the other axis went.
 *
 * What to look for, with the panel upright:
 *   bands top to bottom, black first  -> frame Y is correct
 *   corner wedge at the TOP-LEFT      -> frame X is correct, no mirror
 * Either one wrong pins which axis to invert. */
static void ssd1677_show_color_bars(void)
{
    uint8_t *frame = malloc(EPD_BUF_BYTES);
    if (!frame) {
        ESP_LOGE(TAG, "selftest: out of memory for a %u byte frame",
                 (unsigned)EPD_BUF_BYTES);
        return;
    }
    const int BAND_H = EPD_HEIGHT / 4;
    const int stride = EPD_WIDTH / 4;

    for (int fy = 0; fy < EPD_HEIGHT; fy++) {
        int band = fy / BAND_H;
        if (band > 3) band = 3;
        uint8_t g = (uint8_t)band;              /* 0 = black .. 3 = white */
        uint8_t fill = (uint8_t)((g << 6) | (g << 4) | (g << 2) | g);
        memset(frame + (size_t)fy * stride, fill, stride);
    }
    /* Bottom half: FINE detail, which is the thing bands cannot test.
     *
     * A uniform block has every byte in the row identical, so a wrong stride,
     * a reversed bit order or an off-by-one in the transpose is completely
     * invisible in it -- the bands rendered perfectly while real content came
     * out as snow, which is exactly that gap.
     *
     * Left column pair: 1px vertical black/white stripes, which exercise bit
     * ORDER within a byte. Right: 1px horizontal stripes, which exercise the
     * row STRIDE. Clean lines mean the packing is right; moire, smearing or
     * noise localises which of the two is wrong. */
    for (int fy = EPD_HEIGHT / 2; fy < EPD_HEIGHT; fy++) {
        for (int fx = 0; fx < EPD_WIDTH; fx++) {
            int black = (fx < EPD_WIDTH / 2) ? (fx & 1) : (fy & 1);
            uint8_t g = black ? 0 : 3;
            size_t i = (size_t)fy * stride + (fx >> 2);
            int sh = (3 - (fx & 3)) * 2;
            frame[i] = (uint8_t)((frame[i] & ~(0x3 << sh)) | (g << sh));
        }
    }

    /* Orientation wedge: a WHITE square in the frame's top-left corner.
     *
     * White, not black: it sits inside band 0, which is black, so a black wedge
     * is invisible -- which is exactly how the first version of this shipped.
     * Against band 0 it reads in every orientation, so its position identifies
     * both axes at once however the glass turns out to be mounted. */
    const int WEDGE = (EPD_WIDTH < EPD_HEIGHT ? EPD_WIDTH : EPD_HEIGHT) / 10;
    for (int fy = 0; fy < WEDGE; fy++)
        memset(frame + (size_t)fy * stride, 0xff, WEDGE / 4);

    ssd1677_display(frame);
    free(frame);
}

/* Transport check: alternating pixels across the whole frame. */
static void ssd1677_show_palette_sweep(void)
{
    uint8_t *frame = malloc(EPD_BUF_BYTES);
    if (!frame) return;
    memset(frame, 0x36, EPD_BUF_BYTES);   /* 00 11 01 10 -> visible texture */
    ssd1677_display(frame);
    free(frame);
}

#endif /* EPD_MONO */

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
#ifdef EPD_MONO
        .name      = "SSD1677 (mono 1bpp)",
        .bpp       = 1,
        .grayscale = false,
#else
        .name      = "SSD1677 3.97\" (800x480, 4-gray 2bpp)",
        .bpp       = 2,
        .grayscale = true,
#endif
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
