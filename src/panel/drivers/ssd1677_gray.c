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
#include "../busy_sleep.h"
#include "diag_run.h"           /* BUSY timeouts latch a report for the next status */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

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

/* Update-control byte for a PARTIAL (windowed, differential) refresh: the
 * vendor's partial/DU sequence. Loads temperature, loads the "display mode 2"
 * LUT, displays, and powers down. It is the sequence CrossPoint drives this
 * same panel with for page turns; the incremental 0x1C assembly other SSD16xx
 * drivers use does not select this glass's partial waveform (it runs the full
 * one). Black-and-white only: the mode-2 LUT drives each pixel from the 0x24
 * ("new") vs 0x26 ("old") bit pair, so a partial inside a 4-gray frame is a
 * two-tone patch. */
#define SSD_UPD_PARTIAL 0xff

/* Border waveform while a partial runs. 0x80 holds the border at VCOM; left on
 * the full-refresh value the border ring drives dark on every partial. */
#define SSD_BORDER_PARTIAL 0x80
#define SSD_BORDER_GRAY    0x00

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

/* A board whose glass ships with either an SSD1677 or an SSD2677 (the Sticky)
 * sets EPD_SSD2677_AUTODETECT. The driver then identifies the controller at
 * the first init and routes every entry point to the matching sequence. The
 * SSD2677 half lives in its own section near the end of this file. 4-gray
 * builds only: no mono board carries the part. */
#if defined(EPD_SSD2677_AUTODETECT) && EPD_SSD2677_AUTODETECT && !defined(EPD_MONO)
#  define EPD_HAVE_SSD2677 1
#else
#  define EPD_HAVE_SSD2677 0
#endif

#if EPD_HAVE_SSD2677
#include "esp_attr.h"
#ifdef BOARD_HAS_SHT4X
#include "sht4x.h"
#endif

enum { IC_UNKNOWN = 0, IC_SSD1677 = 1, IC_SSD2677 = 2 };
static uint8_t s_ic = IC_UNKNOWN;
/* Kept across deep sleep so a timer wake skips the probe (and the extra panel
 * reset it costs). Any other reset clears it and the board asks again. */
static RTC_DATA_ATTR uint8_t s_ic_rtc = IC_UNKNOWN;
/* Second handle on the same bus, half-duplex on the data line, for the one
 * read the probe makes. NULL when the bus would not give us one. */
static spi_device_handle_t s_spi_rd;

static bool ssd2677_detect(void);
static void ssd2677_init(void);
static void ssd2677_display(const uint8_t *image);
static void ssd2677_clear(uint8_t color);
static void ssd2677_partial_two_tone(const uint8_t *image, int x, int y, int w, int h);
static void ssd2677_sleep_cmds(void);
#  define IS_SSD2677() (s_ic == IC_SSD2677)
#else
#  define IS_SSD2677() (0)
#endif

/* Reset and power-enable lines. On most boards they are MCU GPIOs: EPD_PIN_RST
 * and the optional EPD_PIN_EN. The M5Stack PaperMono routes both through an
 * M5IOE1 I2C expander instead, so a board may name expander pin indices
 * (EPD_RST_M5IOE1_PIN, EPD_EN_M5IOE1_PIN) and leave the GPIO macros undefined;
 * the GPIO masks below then drop out of gpio_config(). */
#if defined(EPD_RST_M5IOE1_PIN) || defined(EPD_EN_M5IOE1_PIN)
#include "m5ioe1.h"
#endif
#ifdef BOARD_FRONTLIGHT_M5PM1
#include "m5pm1.h"
#endif
#ifdef EPD_RST_M5IOE1_PIN
#  define RST_SET(level)   m5ioe1_set_output(EPD_RST_M5IOE1_PIN, (level))
#  define RST_GPIO_MASK    0ULL
#else
#  define RST_SET(level)   gpio_set_level(EPD_PIN_RST, (level))
#  define RST_GPIO_MASK    (1ULL << EPD_PIN_RST)
#endif
#if defined(EPD_EN_M5IOE1_PIN)
#  define EPD_HAVE_EN      1
#  define EN_SET(level)    m5ioe1_set_output(EPD_EN_M5IOE1_PIN, (level))
#  define EN_GPIO_MASK     0ULL
#elif defined(EPD_PIN_EN)
#  define EPD_HAVE_EN      1
#  define EN_SET(level)    gpio_set_level(EPD_PIN_EN, (level))
#  define EN_GPIO_MASK     (1ULL << EPD_PIN_EN)
#else
#  define EPD_HAVE_EN      0
#  define EN_GPIO_MASK     0ULL
#endif

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
        waited += epd_busy_sleep(EPD_PIN_BUSY, 0, 10);   /* light-sleep until BUSY low; real ms */
        if (!warned && waited >= 10000) {
            ESP_LOGW(TAG, "BUSY still high after 10 s; panel may be stuck");
            warned = true;
        }
        if (waited >= SSD_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "BUSY never cleared in %d ms; continuing so the panel "
                          "still gets powered down", SSD_BUSY_TIMEOUT_MS);
            diag_note_paint(DIAG_PAINT_READY_TIMEOUT);
            return false;
        }
    }
    return true;
}

static void hw_reset(void)
{
    RST_SET(0); vTaskDelay(pdMS_TO_TICKS(10));
    RST_SET(1); vTaskDelay(pdMS_TO_TICKS(10));
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
/* Which plane carries which bit is a property of the glass's OTP waveform,
 * not of the controller. The Sticky's table is above; M5Stack's PaperMono
 * (DEPG0397BBS770F3HP) documents the mid greys the other way round --
 * light gray = (1,0), dark gray = (0,1) -- so a board sets
 * EPD_SSD1677_GRAY_MID_SWAP 1 to exchange the two planes' bits. Black and
 * white are (1,1) and (0,0) either way. */
#ifndef EPD_SSD1677_GRAY_MID_SWAP
#define EPD_SSD1677_GRAY_MID_SWAP 0
#endif
#if EPD_SSD1677_GRAY_MID_SWAP
#define GRAY_P1_BIT(g) (((g) & 1) ^ 1)          /* 0x24 */
#define GRAY_P2_BIT(g) ((((g) >> 1) & 1) ^ 1)   /* 0x26 */
#else
#define GRAY_P1_BIT(g) ((((g) >> 1) & 1) ^ 1)   /* 0x24 */
#define GRAY_P2_BIT(g) (((g) & 1) ^ 1)          /* 0x26 */
#endif

/* The temperature value written before a 4-gray refresh is what selects the
 * grayscale OTP waveform (the controller indexes its LUT table by it). It is
 * per glass: Seeed's Sticky wants 0x67; M5Stack's PaperMono wants 0x5A (their
 * OTP demo's init_gray_mode), and with the wrong one the two planes run
 * through a mono LUT and the panel paints static. */
#ifndef EPD_SSD1677_GRAY_TEMP
#define EPD_SSD1677_GRAY_TEMP 0x67
#endif

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
        .pin_bit_mask = RST_GPIO_MASK | (1ULL << EPD_PIN_DC) |
                        (1ULL << EPD_PIN_CS) | EN_GPIO_MASK,
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
    RST_SET(1);

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

#if EPD_HAVE_SSD2677
    /* The panel has no data-out pin: it answers a read on the data line
     * itself. A 3-wire half-duplex device on the same bus reads it there.
     * Failing to get one is not fatal, the probe then goes by BUSY alone. */
    spi_device_interface_config_t rd = {
        .clock_speed_hz = 1 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX,
    };
    if (spi_bus_add_device(EPD_SPI_HOST, &rd, &s_spi_rd) != ESP_OK) {
        ESP_LOGW(TAG, "no read handle on the panel bus; probing by BUSY only");
        s_spi_rd = NULL;
    }
#endif

    s_port_inited = true;
    return ESP_OK;
}

#ifndef EPD_MONO
/* Partial-refresh state (see the partial section below the paint path). */
#define SHADOW_STRIDE  (EPD_PANEL_SCAN_W / 8)
#define SHADOW_BYTES   ((size_t)SHADOW_STRIDE * EPD_PANEL_SCAN_H)
static uint8_t *s_shadow;              /* 1bpp, controller rows, 1 = white */
static bool     s_shadow_valid;        /* false until the next full paint   */
static bool     s_partial_since_full;  /* a partial ran: temp + border dirty */
static void shadow_rebuild(const uint8_t *image);
#endif

static void ssd1677_init(void)
{
#if EPD_HAVE_EN
    EN_SET(1);    /* panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
#if EPD_HAVE_SSD2677
    if (ssd2677_detect()) { ssd2677_init(); return; }
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
    static const uint8_t TWRITE[] = {EPD_SSD1677_GRAY_TEMP, 0x00};
    cmd_data(SSD_TEMP_WRITE, TWRITE, sizeof TWRITE);
    cmd_data(SSD_RAM_X_ADDR, ZERO2, sizeof ZERO2);
    cmd_data(SSD_RAM_Y_ADDR, ZERO2, sizeof ZERO2);
    wait_idle();

#ifndef EPD_MONO
    /* The shadow describes the GLASS, which keeps its image through a panel
     * power cycle, so it stays valid across init: the overlay path sleeps the
     * panel after every paint and re-inits it before each partial, and the
     * window write below supplies both planes itself, so controller RAM
     * needs no seeding. The MCU's own deep sleep loses the shadow, and the
     * first partial after a boot then falls back to a full paint. */
    s_partial_since_full = false;
#endif
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
#if EPD_HAVE_SSD2677
    if (IS_SSD2677()) { ssd2677_display(image); return; }
#endif
    bus_hold();
#ifdef EPD_MONO
    mono_bypass_second_plane();
    mono_send_plane(image);
    trigger_refresh_mode(SSD_UPD_MONO, "refresh");
#else
    gray_send_plane(SSD_DTM1, image, 1);
    gray_send_plane(SSD_DTM2, image, 2);
    /* A partial refresh (0xFF) reloads the measured temperature and parks the
     * border at VCOM; both would make this gray pass pick the wrong waveform
     * and drive the border oddly, so put the 4-gray selection back first.
     * Cheap, and harmless when nothing changed. */
    {
        static const uint8_t TW[] = {EPD_SSD1677_GRAY_TEMP, 0x00};
        static const uint8_t BG[] = {SSD_BORDER_GRAY};
        cmd_data(SSD_BORDER, BG, sizeof BG);
        cmd_data(SSD_TEMP_WRITE, TW, sizeof TW);
    }
    trigger_refresh();
    shadow_rebuild(image);
    s_partial_since_full = false;
#endif
    bus_release();
}

#if defined(EPD_MONO) && defined(EPD_DUAL_DEPTH)
/* A 2bpp frame on a board whose native frame is 1bpp (EPD_DUAL_DEPTH, the
 * X4). init() is shared and already writes the 4-gray temperature selection;
 * a mono refresh simply reloads the measured one over it. So a grey paint
 * puts RAM2 back in play (the mono path bypasses it), sends both planes as the
 * 4-gray build does, re-selects the 4-gray border and temperature, and
 * refreshes with the 4-gray update byte. No shadow and no partials: the dual
 * build stays a mono build in every other respect. */
static void ssd1677_display_gray(const uint8_t *image)
{
    bus_hold();
    static const uint8_t CTRL1_NORMAL[] = {0x00};
    cmd_data(SSD_CTRL1, CTRL1_NORMAL, sizeof CTRL1_NORMAL);
    gray_send_plane(SSD_DTM1, image, 1);
    gray_send_plane(SSD_DTM2, image, 2);
    static const uint8_t TW[] = {EPD_SSD1677_GRAY_TEMP, 0x00};
    static const uint8_t BG[] = {SSD_BORDER_GRAY};
    cmd_data(SSD_BORDER, BG, sizeof BG);
    cmd_data(SSD_TEMP_WRITE, TW, sizeof TW);
    trigger_refresh();
    bus_release();
}
#endif

static void ssd1677_clear(uint8_t color)
{
#if EPD_HAVE_SSD2677
    if (IS_SSD2677()) { ssd2677_clear(color); return; }
#endif
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

#if defined(EPD_DUAL_DEPTH) && defined(EPD_SELFTEST)
    /* Dual-depth: then the 4-gray ramp through display_gray, black, dark,
     * light, white from the top, with the same white wedge. Even steps between
     * the two greys are what this pass judges. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint8_t *gray = malloc(EPD_GRAY_BUF_BYTES);
    if (!gray) return;
    static const uint8_t LEVEL_BYTE[4] = {0x00, 0x55, 0xAA, 0xFF};
    const int gstride = EPD_WIDTH / 4;
    for (int fy = 0; fy < EPD_HEIGHT; fy++) {
        int band = fy / BAND_H;
        if (band > 3) band = 3;
        memset(gray + (size_t)fy * gstride, LEVEL_BYTE[band], gstride);
    }
    for (int fy = 0; fy < WEDGE; fy++)
        memset(gray + (size_t)fy * gstride, 0xff, WEDGE / 4);
    ssd1677_init();
    ssd1677_display_gray(gray);
    free(gray);
#endif
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
#if EPD_HAVE_SSD2677
    if (IS_SSD2677()) {
        ssd2677_sleep_cmds();
    } else
#endif
    {
        static const uint8_t DEEP[] = {0x01};
        cmd_data(SSD_SLEEP, DEEP, sizeof DEEP);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
#if EPD_HAVE_EN
#  ifdef BOARD_FRONTLIGHT_M5PM1
    /* The PaperMono's frontlight LEDs hang off this same rail: keep it up
     * while a light level is set, or the light only ever shows during a
     * refresh. The controller is already in deep sleep mode 1 by now, so
     * the rail carries only the LEDs and the controller's sleep current. */
    if (!m5pm1_frontlight_active()) EN_SET(0);
#  else
    EN_SET(0);    /* cut panel power */
#  endif
#endif
}

#ifndef EPD_MONO
/* ---------- partial refresh (overlay feature) ----------
 *
 * Windowed two-tone repaint inside the 4-gray frame, for tap echo, touch-v3
 * primitives and frame patches. The controller's partial waveform is a
 * DIFFERENTIAL one: it drives each pixel from the pair (old bit in 0x26, new
 * bit in 0x24), so both planes of the window have to hold black-and-white
 * content when it runs. After a gray paint they hold the two gray planes
 * instead, which is why the driver keeps s_shadow: a 1bpp controller-layout
 * copy of what the glass shows, thresholded from every full paint and updated
 * by every partial, and writes the window's OLD bits from it.
 *
 * Coordinates arrive in FRAME space and go through the same transpose as the
 * full path: a frame rect (x,y,w,h) is controller columns [y, y+h) on rows
 * [x, x+w), widened so the column range is byte-aligned (RAM writes are byte
 * wide along X). The rest of the panel is untouched.
 *
 * Polarity: the mono waveform reads 1 = white, so the threshold is the frame
 * value's high bit (levels 2 and 3 -> white). SSD1677_PARTIAL_INVERT flips it
 * for a glass that comes out negative in the -overlaytest env.
 *
 * Only in the 4-gray build: the mono build's frame is already the plane the
 * waveform wants, and no mono board advertises overlay today. */
#ifndef SSD1677_PARTIAL_INVERT
#define SSD1677_PARTIAL_INVERT 0
#endif

static inline uint8_t mono_bit_of_gray(uint8_t g)
{
    uint8_t b = (uint8_t)((g >> 1) & 1);
    return SSD1677_PARTIAL_INVERT ? (uint8_t)(b ^ 1) : b;
}

/* Frame pixel at CONTROLLER coordinates, through the same mapping as
 * gray_send_plane(): controller row cy, column cx. */
static inline uint8_t scan_px(const uint8_t *image, int cx, int cy)
{
    const int my = mirror_y(cy);
#if EPD_SSD1677_TRANSPOSE
    return frame_px(image, my, cx);
#else
    return frame_px(image, cx, my);
#endif
}

/* Rebuild the shadow from a frame that has just been painted in full. */
static void shadow_rebuild(const uint8_t *image)
{
    if (!s_shadow) {
        s_shadow = heap_caps_malloc(SHADOW_BYTES, TESSERAE_FB_CAPS);
        if (!s_shadow) { ESP_LOGW(TAG, "no memory for the partial shadow"); return; }
    }
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        uint8_t *row = s_shadow + (size_t)cy * SHADOW_STRIDE;
        for (int b = 0; b < SHADOW_STRIDE; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++)
                o = (uint8_t)((o << 1) | mono_bit_of_gray(scan_px(image, b * 8 + k, cy)));
            row[b] = o;
        }
    }
    s_shadow_valid = true;
}

/* RAM window + write counters, in controller pixels. cx0 must be a multiple
 * of 8 and cx1 the last pixel of a whole byte (data entry 0x03: X then Y). */
static void set_ram_window(int cx0, int cx1, int cy0, int cy1)
{
    const uint8_t xw[] = {(uint8_t)(cx0 & 0xff), (uint8_t)(cx0 >> 8),
                          (uint8_t)(cx1 & 0xff), (uint8_t)(cx1 >> 8)};
    const uint8_t yw[] = {(uint8_t)(cy0 & 0xff), (uint8_t)(cy0 >> 8),
                          (uint8_t)(cy1 & 0xff), (uint8_t)(cy1 >> 8)};
    const uint8_t xa[] = {(uint8_t)(cx0 & 0xff), (uint8_t)(cx0 >> 8)};
    const uint8_t ya[] = {(uint8_t)(cy0 & 0xff), (uint8_t)(cy0 >> 8)};
    cmd_data(SSD_RAM_X, xw, sizeof xw);
    cmd_data(SSD_RAM_Y, yw, sizeof yw);
    cmd_data(SSD_RAM_X_ADDR, xa, sizeof xa);
    cmd_data(SSD_RAM_Y_ADDR, ya, sizeof ya);
}

static void ssd1677_partial_two_tone(const uint8_t *image, int x, int y, int w, int h)
{
#if EPD_HAVE_SSD2677
    if (IS_SSD2677()) { ssd2677_partial_two_tone(image, x, y, w, h); return; }
#endif
    /* Clamp to the frame. */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_WIDTH)  w = EPD_WIDTH - x;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    if (!s_shadow_valid) {
        /* Nothing trustworthy to diff against (first paint after init went
         * through some other path): repaint the whole frame instead. */
        ESP_LOGW(TAG, "partial without a shadow; full repaint");
        ssd1677_display(image);
        return;
    }

    /* Frame rect -> controller rect, then byte-align the column range. */
#if EPD_SSD1677_TRANSPOSE
    int cx0 = y, cxw = h, cy0 = x, cyh = w;
#else
    int cx0 = x, cxw = w, cy0 = y, cyh = h;
#endif
    if (EPD_MIRROR_Y) cy0 = EPD_PANEL_SCAN_H - cy0 - cyh;
    int cx1 = cx0 + cxw;                 /* exclusive */
    cx0 &= ~7;
    cx1 = (cx1 + 7) & ~7;
    if (cx1 > EPD_PANEL_SCAN_W) cx1 = EPD_PANEL_SCAN_W;
    const int nb = (cx1 - cx0) / 8;      /* bytes per window row */
    const int cy1 = cy0 + cyh;           /* exclusive */

    int64_t t0 = esp_timer_get_time();
    bus_hold();

    /* Seed BOTH planes, panel-wide, from the shadow first. The partial
     * waveform runs over the whole panel, not just the window we write:
     * every pixel whose 0x24 and 0x26 bits differ gets driven. After a gray
     * paint the RAM holds the two gray planes, which differ for every grey
     * pixel, so a window update dimmed the rest of the page (seen on the
     * bench). With both planes equal to the mono shadow everywhere, every
     * pixel outside the window is "no change" and keeps whatever grey the
     * glass shows. Two plane writes cost about 200 ms on this bus, paid on
     * every partial because the panel is power-cycled between them. */
    set_ram_window(0, EPD_PANEL_SCAN_W - 1, 0, EPD_PANEL_SCAN_H - 1);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++)
        send_data(s_shadow + (size_t)cy * SHADOW_STRIDE, SHADOW_STRIDE);
    gpio_set_level(EPD_PIN_CS, 1);
    set_ram_window(0, EPD_PANEL_SCAN_W - 1, 0, EPD_PANEL_SCAN_H - 1);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM2);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++)
        send_data(s_shadow + (size_t)cy * SHADOW_STRIDE, SHADOW_STRIDE);
    gpio_set_level(EPD_PIN_CS, 1);

    /* New bits into 0x24 for the window only, row by row (the row buffer
     * lives on the stack, in internal RAM, which is what the SPI path wants).
     * 0x26 already holds the old bits from the seed above. */
    set_ram_window(cx0, cx1 - 1, cy0, cy1 - 1);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(SSD_DTM1);
    for (int cy = cy0; cy < cy1; cy++) {
        uint8_t row[EPD_PANEL_SCAN_W / 8];
        for (int b = 0; b < nb; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++)
                o = (uint8_t)((o << 1) | mono_bit_of_gray(scan_px(image, cx0 + b * 8 + k, cy)));
            row[b] = o;
        }
        send_data(row, (size_t)nb);
    }
    gpio_set_level(EPD_PIN_CS, 1);

    /* Now the shadow may take the new bits. */
    for (int cy = cy0; cy < cy1; cy++) {
        uint8_t *sh = s_shadow + (size_t)cy * SHADOW_STRIDE + cx0 / 8;
        for (int b = 0; b < nb; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++)
                o = (uint8_t)((o << 1) | mono_bit_of_gray(scan_px(image, cx0 + b * 8 + k, cy)));
            sh[b] = o;
        }
    }

    static const uint8_t BORDER_P[] = {SSD_BORDER_PARTIAL};
    cmd_data(SSD_BORDER, BORDER_P, sizeof BORDER_P);
    trigger_refresh_mode(SSD_UPD_PARTIAL, "partial");

    /* Leave the window at the full panel for whatever paints next. */
    set_ram_window(0, EPD_PANEL_SCAN_W - 1, 0, EPD_PANEL_SCAN_H - 1);
    bus_release();
    s_partial_since_full = true;
    ESP_LOGI(TAG, "partial (%d,%d %dx%d) -> scan cols %d..%d rows %d..%d in %lld ms",
             x, y, w, h, cx0, cx1 - 1, cy0, cy1 - 1,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void ssd1677_display_partial(const uint8_t *image, int x, int y, int w,
                                    int h, bool fast)
{
    /* No grayscale partial exists on this glass: a quality request is a
     * full 4-gray repaint, which is what the overlay hygiene pass wants. */
    if (fast) ssd1677_partial_two_tone(image, x, y, w, h);
    else      ssd1677_display(image);
}

static void ssd1677_display_partial_mode(const uint8_t *image, int x, int y,
                                         int w, int h, epd_refresh_t mode)
{
    if (mode == EPD_RF_A2 || mode == EPD_RF_DU)
        ssd1677_partial_two_tone(image, x, y, w, h);
    else
        ssd1677_display(image);
}

/* Only the two-tone path with a shadow stays inside the rect; everything else
 * above is a full repaint. The shadow does not survive the MCU's deep sleep. */
static bool ssd1677_partial_rect_safe(epd_refresh_t mode)
{
    return (mode == EPD_RF_A2 || mode == EPD_RF_DU) && s_shadow_valid;
}
#endif /* !EPD_MONO */

#if EPD_HAVE_SSD2677
/* ---------- SSD2677 (later Sticky units) ----------
 *
 * Same glass, same pins, a different controller. Despite the name it does not
 * speak the SSD16xx command set at all. It is driven like a UC8179: panel
 * setting 0x00, power on 0x04, one data command 0x10, refresh 0x12, power off
 * 0x02, deep sleep 0x07. The differences that matter here:
 *
 *   BUSY     active LOW (0 = busy), the opposite of the SSD1677.
 *   Data     ONE stream of 2 bits per pixel, 4 px/byte MSB-first, 200 bytes a
 *            row, where the SSD1677 takes two 1bpp planes.
 *   Waveform chosen from OTP by a code written to 0xE6 and latched with 0xA5,
 *            in temperature buckets. Nothing is uploaded.
 *   Partial  the same 0x10 stream, each pixel carrying (old bit, new bit).
 *            The whole frame is sent; a pixel whose two bits match is left
 *            alone. No RAM window.
 *
 * Sequences and constants follow Seeed's own driver for this part: the init
 * table, the reset timing (20 ms low, 10 ms high), the waveform codes and
 * their temperature buckets, and the 0x70 identity read.
 *
 * NOT YET RUN ON HARDWARE. Three things are inferred and each has a switch:
 * orientation (EPD_SSD2677_MIRROR_X / _Y), the order of the two mid greys
 * (EPD_SSD2677_GRAY_MID_SWAP), and partial polarity (SSD1677_PARTIAL_INVERT,
 * shared with the SSD1677 path). The -selftest env judges the first two, the
 * -overlaytest env the third. */

/* Orientation relative to the SSD1677 path's mapping (controller row cy,
 * column cx reads frame pixel fx = cy, fy = cx on the Sticky). The glass is
 * the same, so the default is no extra mirror. */
#ifndef EPD_SSD2677_MIRROR_X
#define EPD_SSD2677_MIRROR_X 0
#endif
#ifndef EPD_SSD2677_MIRROR_Y
#define EPD_SSD2677_MIRROR_Y 0
#endif

/* The 4-gray waveform's pixel codes are 0 = black, 3 = white, with the mid
 * greys crossed: 2 = dark, 1 = light. The frame is linear (1 = dark, 2 =
 * light), so the two swap on the way out. Set to 0 if the selftest's middle
 * bands come out in the wrong order. */
#ifndef EPD_SSD2677_GRAY_MID_SWAP
#define EPD_SSD2677_GRAY_MID_SWAP 1
#endif

/* Force the choice and skip the probe, for the bench: 1677 or 2677. */
#ifndef EPD_SSD_FORCE_IC
#define EPD_SSD_FORCE_IC 0
#endif

#define S26_PANEL_SETTING 0x00
#define S26_POWER_OFF     0x02
#define S26_POWER_ON      0x04
#define S26_BOOSTER       0x06
#define S26_DEEP_SLEEP    0x07
#define S26_DATA          0x10
#define S26_REFRESH       0x12
#define S26_PLL           0x30
#define S26_CDI           0x50
#define S26_RESOLUTION    0x61
#define S26_TIMING        0x62
#define S26_GSST          0x65
#define S26_READ_ID       0x70
#define S26_WF_LATCH      0xa5
#define S26_WF_MODE       0xe0
#define S26_WF_CODE       0xe6
#define S26_ID            0x07   /* what an SSD2677 answers to 0x70 */

#define S26_ROW_BYTES     (EPD_PANEL_SCAN_W / 4)
#define S26_BUSY_TIMEOUT_MS 45000

static bool s26_powered;   /* high voltage on: a partial leaves it up */

/* BUSY low = busy. Bounded like wait_idle(), for the same reason. */
static bool s26_wait_ready(void)
{
    int waited = 0;
    bool warned = false;
    while (gpio_get_level(EPD_PIN_BUSY) == 0) {
        waited += epd_busy_sleep(EPD_PIN_BUSY, 1, 10);
        if (!warned && waited >= 10000) {
            ESP_LOGW(TAG, "SSD2677: BUSY still low after 10 s; panel may be stuck");
            warned = true;
        }
        if (waited >= S26_BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "SSD2677: BUSY never released in %d ms; continuing so "
                          "the panel still gets powered down", S26_BUSY_TIMEOUT_MS);
            diag_note_paint(DIAG_PAINT_READY_TIMEOUT);
            return false;
        }
    }
    return true;
}

static void s26_reset(void)
{
    RST_SET(0); vTaskDelay(pdMS_TO_TICKS(20));
    RST_SET(1); vTaskDelay(pdMS_TO_TICKS(10));
}

/* Send 0x70 and read one byte back on the data line. 0 when the bus gave us
 * no read handle or the transfer failed, which the caller treats as "no
 * answer" and settles by BUSY instead. */
static uint8_t s26_read_id(void)
{
    if (!s_spi_rd) return 0;
    uint8_t id = 0;

    spi_device_acquire_bus(s_spi_rd, portMAX_DELAY);
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    spi_transaction_t tx = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .tx_data = {S26_READ_ID},
    };
    esp_err_t err = spi_device_polling_transmit(s_spi_rd, &tx);
    if (err == ESP_OK) {
        gpio_set_level(EPD_PIN_DC, 1);
        spi_transaction_t rx = {
            .flags = SPI_TRANS_USE_RXDATA,
            .rxlength = 8,
        };
        err = spi_device_polling_transmit(s_spi_rd, &rx);
        if (err == ESP_OK) id = rx.rx_data[0];
    }
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_DC, 0);
    spi_device_release_bus(s_spi_rd);

    if (err != ESP_OK) ESP_LOGW(TAG, "identity read failed: %s", esp_err_to_name(err));
    return id;
}

/* Which controller is behind the glass? Panel power must already be up.
 *
 * Two independent signs, either of which selects the SSD2677:
 *   - it answers 0x07 to command 0x70 (Seeed's own test);
 *   - BUSY rests HIGH once the reset has settled. The SSD1677 rests LOW,
 *     because the two parts use opposite BUSY polarity.
 * The second covers a read that returns nothing, and it is the same symptom
 * the SSD1677 sequence shows on the new part ("BUSY still high after 10 s").
 * Sampled 150 ms after reset so neither part is still in its reset busy
 * phase, where each looks like the other. */
static bool ssd2677_detect(void)
{
    if (s_ic != IC_UNKNOWN) return s_ic == IC_SSD2677;

#if EPD_SSD_FORCE_IC == 2677
    s_ic = IC_SSD2677;
    ESP_LOGW(TAG, "controller forced to SSD2677 at build time");
#elif EPD_SSD_FORCE_IC == 1677
    s_ic = IC_SSD1677;
    ESP_LOGW(TAG, "controller forced to SSD1677 at build time");
#else
    if (s_ic_rtc == IC_SSD1677 || s_ic_rtc == IC_SSD2677) {
        s_ic = s_ic_rtc;
        return s_ic == IC_SSD2677;
    }
    s26_reset();
    const uint8_t id = s26_read_id();
    vTaskDelay(pdMS_TO_TICKS(150));
    const int busy = gpio_get_level(EPD_PIN_BUSY);
    s_ic = (id == S26_ID || busy) ? IC_SSD2677 : IC_SSD1677;
    /* Always logged: a wrong pick shows up as a BUSY timeout, and this line
     * is what says why. */
    ESP_LOGI(TAG, "controller probe: 0x70 -> 0x%02x, BUSY rests %s: %s",
             id, busy ? "high" : "low",
             s_ic == IC_SSD2677 ? "SSD2677" : "SSD1677");
#endif
    s_ic_rtc = s_ic;
    return s_ic == IC_SSD2677;
}

static void ssd2677_init(void)
{
    s26_reset();
    s26_wait_ready();

    static const uint8_t PSR[]     = {0x2f, 0x0e};
    static const uint8_t BOOSTER[] = {0x0f, 0x8b, 0x93, 0xc1};
    static const uint8_t E7[]      = {0xc1};
    static const uint8_t PLL[]     = {0x08};
    static const uint8_t CDI[]     = {0x77};
    static const uint8_t TIMING[]  = {0x76, 0x76, 0x76, 0x5a, 0x9d, 0x8a, 0x76, 0x62};
    /* 800 x 680 as Seeed writes it, although 480 rows are sent. The value is
     * theirs and is kept as is. */
    static const uint8_t RES[]     = {0x03, 0x20, 0x02, 0xa8};
    static const uint8_t WF_INIT[] = {0x10};
    static const uint8_t GSST[]    = {0x00, 0x00, 0x00, 0x00};
    static const uint8_t E9[]      = {0x01};

    cmd_data(S26_PANEL_SETTING, PSR, sizeof PSR);
    s26_wait_ready();
    cmd_data(S26_BOOSTER, BOOSTER, sizeof BOOSTER);
    cmd_data(0xe7, E7, sizeof E7);
    cmd_data(S26_PLL, PLL, sizeof PLL);
    cmd_data(S26_CDI, CDI, sizeof CDI);
    cmd_data(S26_TIMING, TIMING, sizeof TIMING);
    cmd_data(S26_RESOLUTION, RES, sizeof RES);
    cmd_data(S26_WF_MODE, WF_INIT, sizeof WF_INIT);
    cmd_data(S26_GSST, GSST, sizeof GSST);
    cmd_data(0xe9, E9, sizeof E9);
    s26_wait_ready();

    s26_powered = false;
    s_partial_since_full = false;   /* the shadow survives; see ssd1677_init() */
    ESP_LOGI(TAG, "init complete (SSD2677 4-gray, scan %dx%d, frame %dx%d)",
             EPD_PANEL_SCAN_W, EPD_PANEL_SCAN_H, EPD_WIDTH, EPD_HEIGHT);
}

/* Ambient temperature for the waveform bucket, whole degrees, 0..127. From
 * the board's SHT40 rather than the controller's own sensor (command 0x40),
 * which would need a second read on the data line in the middle of a paint.
 * Seeed's driver takes an external reading the same way when it is given one.
 * 25 when the sensor does not answer, which lands in the room-temperature
 * bucket of every table below. */
static uint8_t s26_temperature(void)
{
#ifdef BOARD_HAS_SHT4X
    sht4x_sample_t s;
    if (sht4x_read(&s) == ESP_OK) {
        if (!(s.temperature_c > 0.0f)) return 0;
        if (s.temperature_c >= 127.0f) return 127;
        return (uint8_t)(s.temperature_c + 0.5f);
    }
#endif
    return 25;
}

static uint8_t s26_gray_code(uint8_t t)    { return t <= 15 ? 0xf1 : 0xfa; }
static uint8_t s26_partial_code(uint8_t t)
{
    if (t <= 10) return 0x08;
    if (t <= 20) return 0x0f;
    if (t <= 30) return 0x19;
    return 0x23;
}

/* Select the OTP waveform for the frame that follows. */
static void s26_latch_waveform(uint8_t code)
{
    static const uint8_t MODE[] = {0x12};
    cmd_data(S26_WF_MODE, MODE, sizeof MODE);
    cmd_data(S26_WF_CODE, &code, 1);
    cmd_data(S26_WF_LATCH, NULL, 0);
    s26_wait_ready();
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* Power on if needed, refresh, and optionally power off again. A partial
 * keeps the high voltage up so the next one starts sooner; sleep drops it. */
static void s26_refresh(bool power_off, const char *what)
{
    static const uint8_t ZERO[] = {0x00};
    int64_t t0 = esp_timer_get_time();
    if (!s26_powered) {
        cmd_data(S26_POWER_ON, NULL, 0);
        s26_wait_ready();
        s26_powered = true;
    }
    cmd_data(S26_REFRESH, ZERO, sizeof ZERO);
    s26_wait_ready();
    if (power_off) {
        cmd_data(S26_POWER_OFF, ZERO, sizeof ZERO);
        s26_wait_ready();
        s26_powered = false;
    }
    ESP_LOGI(TAG, "SSD2677 %s done (%lld ms)", what,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

/* Frame coordinates of controller pixel (cx, cy). */
static inline void s26_frame_xy(int cx, int cy, int *fx, int *fy)
{
    const int mx = EPD_SSD2677_MIRROR_X ? (EPD_PANEL_SCAN_W - 1 - cx) : cx;
    const int my = EPD_SSD2677_MIRROR_Y ? (EPD_PANEL_SCAN_H - 1 - cy) : cy;
#if EPD_SSD1677_TRANSPOSE
    *fx = my; *fy = mx;
#else
    *fx = mx; *fy = my;
#endif
}

/* Frame level (0 = black .. 3 = white) to the waveform's pixel code. */
static inline uint8_t s26_level(uint8_t g)
{
#if EPD_SSD2677_GRAY_MID_SWAP
    static const uint8_t MAP[4] = {0, 2, 1, 3};
    return MAP[g & 3];
#else
    return (uint8_t)(g & 3);
#endif
}

/* Rebuild the 1bpp shadow (controller layout, 1 = white) from a frame that
 * has just been painted in full, through THIS controller's mapping. */
static void s26_shadow_rebuild(const uint8_t *image)
{
    if (!s_shadow) {
        s_shadow = heap_caps_malloc(SHADOW_BYTES, TESSERAE_FB_CAPS);
        if (!s_shadow) { ESP_LOGW(TAG, "no memory for the partial shadow"); return; }
    }
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        uint8_t *row = s_shadow + (size_t)cy * SHADOW_STRIDE;
        for (int b = 0; b < SHADOW_STRIDE; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 8; k++) {
                int fx, fy;
                s26_frame_xy(b * 8 + k, cy, &fx, &fy);
                o = (uint8_t)((o << 1) | mono_bit_of_gray(frame_px(image, fx, fy)));
            }
            row[b] = o;
        }
    }
    s_shadow_valid = true;
}

static void ssd2677_display(const uint8_t *image)
{
    uint8_t row[S26_ROW_BYTES];

    bus_hold();
    s26_latch_waveform(s26_gray_code(s26_temperature()));

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(S26_DATA);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        for (int b = 0; b < S26_ROW_BYTES; b++) {
            uint8_t o = 0;
            for (int k = 0; k < 4; k++) {
                int fx, fy;
                s26_frame_xy(b * 4 + k, cy, &fx, &fy);
                o = (uint8_t)((o << 2) | s26_level(frame_px(image, fx, fy)));
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);

    s26_refresh(true, "refresh");
    s26_shadow_rebuild(image);
    s_partial_since_full = false;
    bus_release();
}

static void ssd2677_clear(uint8_t color)
{
    const uint8_t g = (uint8_t)(color & 0x3);
    uint8_t row[S26_ROW_BYTES];
    memset(row, (uint8_t)(s26_level(g) * 0x55), sizeof row);

    bus_hold();
    s26_latch_waveform(s26_gray_code(s26_temperature()));
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(S26_DATA);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) send_data(row, sizeof row);
    gpio_set_level(EPD_PIN_CS, 1);
    s26_refresh(true, "clear");
    if (s_shadow) {
        memset(s_shadow, mono_bit_of_gray(g) ? 0xff : 0x00, SHADOW_BYTES);
        s_shadow_valid = true;
    }
    s_partial_since_full = false;
    bus_release();
}

/* Two-tone repaint of a frame rect. The controller takes the whole frame as
 * (old, new) bit pairs and drives only the pixels whose pair differs, so
 * outside the rect both bits come from the shadow and the glass keeps
 * whatever grey it shows. */
static void ssd2677_partial_two_tone(const uint8_t *image, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > EPD_WIDTH)  w = EPD_WIDTH - x;
    if (y + h > EPD_HEIGHT) h = EPD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    if (!s_shadow_valid) {
        ESP_LOGW(TAG, "partial without a shadow; full repaint");
        ssd2677_display(image);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    uint8_t row[S26_ROW_BYTES];

    bus_hold();
    s26_latch_waveform(s26_partial_code(s26_temperature()));

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(S26_DATA);
    for (int cy = 0; cy < EPD_PANEL_SCAN_H; cy++) {
        uint8_t *sh = s_shadow + (size_t)cy * SHADOW_STRIDE;
        for (int b = 0; b < SHADOW_STRIDE; b++) {
            const uint8_t oldb = sh[b];
            uint8_t newb = oldb;
            for (int k = 0; k < 8; k++) {
                int fx, fy;
                s26_frame_xy(b * 8 + k, cy, &fx, &fy);
                if (fx < x || fx >= x + w || fy < y || fy >= y + h) continue;
                const uint8_t m = (uint8_t)(0x80 >> k);
                if (mono_bit_of_gray(frame_px(image, fx, fy))) newb |= m;
                else                                           newb &= (uint8_t)~m;
            }
            /* 8 pixels of old + new -> 16 bits, old in the high bit of each
             * pair, first pixel in the top bits. */
            uint16_t v = 0;
            for (int i = 7; i >= 0; i--)
                v = (uint16_t)((v << 2) | (((oldb >> i) & 1) << 1) | ((newb >> i) & 1));
            row[b * 2]     = (uint8_t)(v >> 8);
            row[b * 2 + 1] = (uint8_t)v;
            sh[b] = newb;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);

    s26_refresh(false, "partial");
    bus_release();
    s_partial_since_full = true;
    ESP_LOGI(TAG, "SSD2677 partial (%d,%d %dx%d) in %lld ms", x, y, w, h,
             (long long)((esp_timer_get_time() - t0) / 1000));
}

/* Controller half of sleep; the caller drops the panel rail afterwards. */
static void ssd2677_sleep_cmds(void)
{
    static const uint8_t ZERO[]  = {0x00};
    static const uint8_t CHECK[] = {0xa5};
    if (s26_powered) {
        cmd_data(S26_POWER_OFF, ZERO, sizeof ZERO);
        s26_wait_ready();
        s26_powered = false;
    }
    cmd_data(S26_DEEP_SLEEP, CHECK, sizeof CHECK);
}
#endif /* EPD_HAVE_SSD2677 */

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
#if defined(EPD_MONO) && defined(EPD_DUAL_DEPTH)
    .display_gray       = ssd1677_display_gray,
#endif
    .show_color_bars    = ssd1677_show_color_bars,
    .show_palette_sweep = ssd1677_show_palette_sweep,
    .sleep              = ssd1677_sleep,
#ifndef EPD_MONO
    .display_partial      = ssd1677_display_partial,
    .display_partial_mode = ssd1677_display_partial_mode,
    .partial_rect_safe    = ssd1677_partial_rect_safe,
#endif
};

#endif /* PANEL_DRIVER_SSD1677_GRAY */
