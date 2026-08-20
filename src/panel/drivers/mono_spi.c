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
#include "esp_timer.h"
#if defined(EPD_GRAY4)
/* Bit-banging SCLK/MOSI for the OTP probe means detaching them from the SPI
 * peripheral and putting them back afterwards, which is GPIO-matrix work. */
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_sig_map.h"
#include "soc/spi_periph.h"
#endif
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
/* Treat the panel as idle only after BUSY reads high on this many consecutive
 * samples. A single glitch high part-way through a multi-second refresh would
 * otherwise end the wait early and let the caller cut power mid-waveform. */
#define BUSY_IDLE_STABLE_SAMPLES 3

/* How long to allow for BUSY to ASSERT after a command that starts long work.
 * Normal assertion is a few ms; this only bounds the pathological case. */
#define BUSY_ASSERT_TIMEOUT_MS 1000

/* Wait for BUSY to clear. BUSY is active-LOW (0 = busy). */
/* Hard ceiling on any BUSY wait. The longest legitimate operation is a 4-gray
 * full refresh, ~20 s even at the tuner's maximum drive, so 45 s cannot cut a
 * healthy refresh short.
 *
 * It MUST be bounded. This loop previously warned at 60 s and then waited for
 * ever, which on a panel whose BUSY never clears means POF and DSLP are never
 * sent -- and on boards with no panel power-enable pin (the E1001 has none)
 * nothing else can cut the drive. The panel is then left energised
 * indefinitely, and a static field across the ink relaxes the image back to a
 * washed-out grey shortly after every otherwise-correct refresh. That is the
 * field report exactly, including why cutting power at the wall preserves full
 * contrast: it removes the field the firmware failed to remove.
 *
 * Timing out and CONTINUING is the safe failure. Powering the panel down after
 * a possibly-incomplete refresh costs one bad frame; hanging with it driven
 * costs the image and, over time, the panel. */
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

/* Wait for the panel to ASSERT busy, i.e. to acknowledge that the work it was
 * just told to do has actually started. Returns false if it never did.
 *
 * This closes a real race. wait_idle() only waits WHILE busy, so if the
 * controller has not pulled BUSY low yet it returns instantly, the caller sends
 * POF, and power is cut part-way through the waveform -- leaving a partial or
 * inverted image on the glass. It is intermittent by nature, and worse on the
 * 4-gray path, which queues register LUTs and two full planes before the
 * refresh begins, so its assertion latency is longer and more variable than
 * mono's. The old fixed 1 ms delay was a guess at that latency; this waits for
 * the event instead of assuming a bound on it. */
static bool wait_busy_asserted(void)
{
    for (int waited = 0; waited < BUSY_ASSERT_TIMEOUT_MS; waited += 2) {
        if (gpio_get_level(EPD_PIN_BUSY) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

static void hw_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

#ifdef EPD_GRAY4
/* ------------------------------------------------------------------ */
/* Which 4-gray waveform this glass wants -- asked of the panel itself.
 *
 * Two batches of E1001 glass are in the field. Newer panels carry a BUILT-IN
 * OTP 4-gray waveform, selected with PSR 0x1F plus a fixed temperature reading;
 * older ones have no such table and can only be driven from register LUTs
 * uploaded over SPI (PSR 0x3F). Choosing wrong gives either no refresh at all
 * or a badly formed image, and until now we chose at BUILD time -- two firmware
 * targets, two device kinds, and an operator flashing both to see which one lit
 * up. The board header even claimed this was unavoidable ("the panel is
 * write-only, so nothing can probe which glass is fitted").
 *
 * It is not. The UC8179 SDA line is bidirectional and the controller will read
 * back its own OTP, which records whether a 4-gray table was burned in. Seeed's
 * own driver does exactly this (uc8179ProbeOtpSupport in Seeed_GFX's
 * TFT_eSPI.cpp), which is how the stock firmware serves both batches from one
 * image. This is a port of that probe.
 *
 * The SPI peripheral cannot do the read: SDA is wired to MOSI alone (MISO on
 * these boards belongs to the SD card), so it is bit-banged with MOSI turned
 * around. The SD card shares SCLK and MOSI, so the bus lock is held across the
 * whole probe and the pins are put back on the peripheral afterwards.
 *
 * Cost is one ~200 ms read per boot, in mono_port_init(), which is idempotent.
 */

/* Build-time override: -1 probes the panel, 0 forces register LUTs, 1 forces
 * the built-in OTP waveform. Kept as an escape hatch for a panel that ever
 * needs overriding; no board pins it any more. */
#ifndef GRAY4_WAVEFORM
#ifdef EPD_GRAY4_REG_LUTS
#define GRAY4_WAVEFORM 0
#else
#define GRAY4_WAVEFORM (-1)
#endif
#endif

/* What to use when the probe cannot answer: 1 = built-in OTP, 0 = register LUTs.
 *
 * This is per board, and it matters most where the two populations differ. A
 * device that has been running the legacy-glass target is KNOWN to be on glass
 * with no built-in table, so falling back to OTP there would break a unit that
 * works today. Its board header sets 0 and keeps that guarantee without pinning
 * the answer, which is what lets it take the probe at all.
 *
 * Everything else defaults to 1, the pre-probe shipped behaviour, so a probe
 * that cannot run is "no new information" rather than "new behaviour". Note
 * that both directions are the opposite of Seeed, who always fall back to
 * register LUTs; ours follows the field evidence per target instead. */
#ifndef GRAY4_WAVEFORM_FALLBACK
#define GRAY4_WAVEFORM_FALLBACK 1
#endif

/* What the panel said, seeded with the fallback above. */
static bool s_gray_use_otp = (GRAY4_WAVEFORM_FALLBACK != 0);

#define GRAY_OTP_CLK_US   1        /* ~500 kHz, near Seeed's bit-bang rate */
#define GRAY_OTP_BUSY_MS  3000

/* OTP dump geometry: clock out `total` bytes and keep 10 from `skip`. These are
 * positions in the dump rather than documented register addresses -- they come
 * from Seeed's driver and there is no datasheet to re-derive them from, so do
 * not "tidy" them. Two locations are read because the flag lives at different
 * offsets across panel revisions; either one saying 0x01 is enough. */
#define GRAY_OTP_A_TOTAL  0x0BED
#define GRAY_OTP_A_SKIP   0x0BE3
#define GRAY_OTP_B_TOTAL  0x17ED
#define GRAY_OTP_B_SKIP   0x17E3

/* Take SCLK/MOSI off the SPI peripheral and drive them as plain GPIOs.
 * gpio_set_direction() alone is NOT enough: it flips the output enable but
 * leaves the peripheral's signal routed through the GPIO matrix, so the pin
 * would keep following SPI and every bit we clocked would be ignored. */
static void gray_otp_bus_detach(void)
{
    esp_rom_gpio_connect_out_signal(EPD_PIN_SCLK, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(EPD_PIN_MOSI, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)EPD_PIN_SCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_MOSI, 1);
}

/* Put them back exactly the way spi_common wires them for a master. */
static void gray_otp_bus_reattach(void)
{
    const spi_signal_conn_t *sig = &spi_periph_signal[EPD_SPI_HOST];

    gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction((gpio_num_t)EPD_PIN_SCLK, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(EPD_PIN_MOSI, sig->spid_out, false, false);
    esp_rom_gpio_connect_in_signal(EPD_PIN_MOSI, sig->spid_in, false);
    esp_rom_gpio_connect_out_signal(EPD_PIN_SCLK, sig->spiclk_out, false, false);
    esp_rom_gpio_connect_in_signal(EPD_PIN_SCLK, sig->spiclk_in, false);
}

/* BUSY is active low; the probe wants it HIGH (idle) before each transfer. */
static bool gray_otp_wait_ready(void)
{
    for (int waited = 0; waited < GRAY_OTP_BUSY_MS; waited += 10) {
        if (gpio_get_level(EPD_PIN_BUSY)) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* Seeed resets before every read in the probe; the controller will not accept a
 * second read command without one. Their timings, not hw_reset()'s. */
static void gray_otp_reset(void)
{
    gpio_set_level(EPD_PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

static void gray_otp_cmd(uint8_t cmd)
{
    gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_level(EPD_PIN_DC, 0);
    for (int i = 0; i < 8; i++) {
        gpio_set_level(EPD_PIN_MOSI, (cmd & 0x80) ? 1 : 0);
        cmd = (uint8_t)(cmd << 1);
        esp_rom_delay_us(GRAY_OTP_CLK_US);
        gpio_set_level(EPD_PIN_SCLK, 1);
        esp_rom_delay_us(GRAY_OTP_CLK_US);
        gpio_set_level(EPD_PIN_SCLK, 0);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* One CS-framed read byte, MSB first, sampled while SCLK is high. */
static uint8_t gray_otp_read_byte(void)
{
    uint8_t v = 0;

    gpio_set_level(EPD_PIN_CS, 0);
    gpio_set_level(EPD_PIN_DC, 1);
    gpio_set_level(EPD_PIN_SCLK, 0);
    gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_INPUT);
    for (int i = 0; i < 8; i++) {
        v = (uint8_t)(v << 1);
        esp_rom_delay_us(GRAY_OTP_CLK_US);
        gpio_set_level(EPD_PIN_SCLK, 1);
        if (gpio_get_level(EPD_PIN_MOSI)) v |= 1;
        esp_rom_delay_us(GRAY_OTP_CLK_US);
        gpio_set_level(EPD_PIN_SCLK, 0);
    }
    gpio_set_direction((gpio_num_t)EPD_PIN_MOSI, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PIN_MOSI, 1);
    gpio_set_level(EPD_PIN_CS, 1);
    return v;
}

/* Dump the OTP (command 0xA2) and keep the window we care about. The whole
 * prefix has to be clocked out; there is no seek. */
static void gray_otp_read_window(uint16_t total, uint16_t skip,
                                 uint8_t *buf, size_t len)
{
    size_t got = 0;

    memset(buf, 0, len);
    gray_otp_cmd(0xa2);
    for (uint16_t i = 0; i < total; i++) {
        uint8_t b = gray_otp_read_byte();
        if (i >= skip && got < len) buf[got++] = b;
    }
}

static void gray_probe_waveform(void)
{
#if GRAY4_WAVEFORM >= 0
    s_gray_use_otp = (GRAY4_WAVEFORM != 0);
    ESP_LOGI(TAG, "4-gray waveform pinned by the board: %s",
             s_gray_use_otp ? "built-in OTP" : "register LUTs");
#else
    uint8_t a[10], b[10], t_int = 0, t_frac = 0;
    bool complete = false;

    esp_err_t lock = spi_device_acquire_bus(s_spi, portMAX_DELAY);
    if (lock != ESP_OK) {
        ESP_LOGW(TAG, "4-gray probe skipped, SPI bus busy (%s); keeping %s",
                 esp_err_to_name(lock),
                 s_gray_use_otp ? "built-in OTP" : "register LUTs");
        return;
    }
    gray_otp_bus_detach();

    /* The on-die temperature read is not part of the decision, but it is part
     * of Seeed's working sequence and it costs two bytes, so it stays. It is
     * also a cheap sanity check on the read path: a panel returning 0x00/0x00
     * here is very likely returning nothing at all below. */
    gray_otp_reset();
    if (gray_otp_wait_ready()) {
        gray_otp_cmd(0x40);
        if (gray_otp_wait_ready()) {
            t_int  = gray_otp_read_byte();
            t_frac = gray_otp_read_byte();
        }
        gray_otp_reset();
        if (gray_otp_wait_ready()) {
            gray_otp_read_window(GRAY_OTP_A_TOTAL, GRAY_OTP_A_SKIP, a, sizeof a);
            gray_otp_reset();
            if (gray_otp_wait_ready()) {
                gray_otp_read_window(GRAY_OTP_B_TOTAL, GRAY_OTP_B_SKIP,
                                     b, sizeof b);
                complete = true;
            }
        }
    }

    gray_otp_bus_reattach();
    spi_device_release_bus(s_spi);

    if (!complete) {
        ESP_LOGW(TAG, "4-gray probe did not finish (BUSY never went idle); "
                      "keeping %s",
                 s_gray_use_otp ? "built-in OTP" : "register LUTs");
        return;
    }

    s_gray_use_otp = (a[0] == 0x01) || (b[0] == 0x01);
    ESP_LOGI(TAG, "4-gray probe: otp[%04x]=%02x otp[%04x]=%02x temp=%u.%u -> %s",
             GRAY_OTP_A_SKIP, a[0], GRAY_OTP_B_SKIP, b[0],
             t_int, t_frac,
             s_gray_use_otp ? "built-in OTP waveform" : "register LUTs");
#endif
}
#endif /* EPD_GRAY4 */

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

#ifdef EPD_GRAY4
    /* Ask the glass which waveform it has before anything tries to drive it.
     * Here rather than in mono_init() because mono_init() runs before every
     * refresh, and the answer is a property of the panel, not of the frame. */
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 1);   /* the probe talks to the panel */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    gray_probe_waveform();
#endif

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
 * Either waveform sends BOTH image planes -- DTM1 (0x10, "old") and DTM2
 * (0x13, "new") -- so each pixel carries 2 bits that land on four optical
 * states in one refresh. On glass with no built-in 4-gray table (see
 * gray_probe_waveform) the waveform itself is uploaded as register LUTs and
 * PSR selects them; the encoding below is the register-LUT one:
 *
 *        white  light-gray  dark-gray  black          (g = 3    2    1    0)
 *   0x10:  1        0           1        0            (bit =  g       & 1)
 *   0x13:  1        1           0        0            (bit = (g >> 1) & 1)
 *
 * Wire format in: 2bpp packed, 4 px/byte, MSB-first (bits 7-6 = leftmost),
 * 0b00 = black .. 0b11 = white, 96000 bytes. 4-gray is always a full refresh.
 *
 * LUTs and init values are Seeed_GFX's (TFT_Drivers/UC8179_Defines.h, the
 * driver behind the stock reTerminal firmware), REPLACING the GoodDisplay
 * GDEY075T7 demo tables this shipped with. Those were not a weaker version of
 * these -- they are a different waveform: four populated phase groups with the
 * last three all zeros, against seven populated groups here. The zeros are why
 * the panel never settled, why every drive-time multiplier changed nothing, and
 * ultimately why a temperature-compensation layer got built on top. There is
 * nothing to compensate now: this is the waveform the working firmware uses. */
static const uint8_t LUT_VCOM_4G[42] = {   /* R20 */
    0x00, 0x00, 0x06, 0x08, 0x07, 0x01,
    0x00, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x00, 0x03, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x05, 0x09, 0x06, 0x06, 0x01,
    0x00, 0x02, 0x02, 0x0A, 0x0A, 0x01,
    0x00, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x00, 0x02, 0x01, 0x02, 0x01, 0x01,
};
static const uint8_t LUT_WW_4G[42] = {   /* R21, white -> white */
    0x15, 0x00, 0x06, 0x08, 0x07, 0x01,
    0x54, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03,
    0x2A, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xAA, 0x02, 0x02, 0x0A, 0x0A, 0x01,
    0x00, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x28, 0x02, 0x01, 0x02, 0x01, 0x01,
};
static const uint8_t LUT_BW_4G[42] = {   /* R22, black -> white (KW) */
    0x2A, 0x00, 0x06, 0x08, 0x07, 0x01,
    0x59, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03,
    0x5A, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xA8, 0x02, 0x02, 0x0A, 0x0A, 0x01,
    0x45, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0xA8, 0x02, 0x01, 0x02, 0x01, 0x01,
};
static const uint8_t LUT_WB_4G[42] = {   /* R23, white -> black (WK) */
    0x16, 0x00, 0x06, 0x08, 0x07, 0x01,
    0xA0, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03,
    0x99, 0x05, 0x09, 0x06, 0x06, 0x01,
    0xA0, 0x02, 0x02, 0x0A, 0x0A, 0x01,
    0x40, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x20, 0x02, 0x01, 0x02, 0x01, 0x01,
};
static const uint8_t LUT_BB_4G[42] = {   /* R24, black -> black (KK) */
    0x26, 0x00, 0x06, 0x08, 0x07, 0x01,
    0x6A, 0x06, 0x0A, 0x0B, 0x0A, 0x01,
    0x90, 0x03, 0x03, 0x00, 0x00, 0x03,
    0x65, 0x05, 0x09, 0x06, 0x06, 0x01,
    0x50, 0x02, 0x02, 0x0A, 0x0A, 0x01,
    0x10, 0x0A, 0x11, 0x06, 0x07, 0x01,
    0x10, 0x02, 0x01, 0x02, 0x01, 0x01,
};
/* Uniform drive-time gain for the register LUTs, in percent. 100 means "as
 * Seeed publishes them", and that is now the shipped value on every build.
 *
 * This knob and its temperature-driven twin both existed to rescue the
 * GoodDisplay tables, whose settling phases were empty: nothing the panel did
 * afterwards could hold the image, so the only lever left looked like drive
 * time. It was never the lever -- stretching it to 150 % changed nothing in the
 * field, which is the result that should have condemned the tables rather than
 * the theory. With Seeed's waveform there is nothing to rescue, so the
 * automatic scaling is gone: no SHT4x read, no ambient-dependent behaviour, the
 * same bytes down the wire every refresh.
 *
 * What remains is a manual override for the on-panel tuner build, which needs
 * to sweep something. Scaling is applied to ALL FIVE LUTs identically, VCOM
 * included: the common plane has to stay time-aligned with the pixel planes, so
 * stretching the pixel LUTs alone desynchronises VCOM and makes the result
 * worse rather than darker. */
#ifndef GRAY4_LUT_GAIN_PCT
#define GRAY4_LUT_GAIN_PCT 100
#endif

/* Send one 42-byte register LUT, scaling only the four phase-duration bytes of
 * each 6-byte group. The level byte (which rail each phase drives to) and the
 * repeat count are structural and must not be touched. */
static void gray_send_lut(uint8_t cmd, const uint8_t *lut, int gain_pct)
{
    if (gain_pct == 100) { cmd_data(cmd, lut, 42); return; }
    uint8_t scaled[42];
    for (int g = 0; g < 7; g++) {
        const uint8_t *src = lut + g * 6;
        uint8_t *dst = scaled + g * 6;
        dst[0] = src[0];                      /* phase level selector */
        for (int ph = 1; ph <= 4; ph++) {     /* four phase durations */
            int v = src[ph] * gain_pct / 100;
            dst[ph] = (uint8_t)(v > 255 ? 255 : v);
        }
        dst[5] = src[5];                      /* repeat count */
    }
    cmd_data(cmd, scaled, 42);
}

/* Register-LUT path init values, all from Seeed_GFX's EPD_INIT_GRAY.
 *
 * PWR is FIVE bytes here, not the four the mono path sends, and byte 1 is 0x17
 * rather than 0x07. Byte 4 is VDHR, which mono leaves at its OTP value; the
 * gray waveform drives the border and the "same level" transitions from that
 * rail, so it is not optional on this path.
 *
 * The booster is the other correction: this shipped as 17 17 28 17, mono's
 * setting, against Seeed's 27 27 28 17. Bytes 0 and 1 are the soft-start drive
 * strength and minimum off time for phases A and B, so ours started the rails
 * softer than the stock firmware does and then asked them to hold up through a
 * waveform three times longer than mono's. Faint output with erratic flashing
 * is what an undersupplied rail looks like. */
static const uint8_t PWR_4G[]  = {0x07, 0x17, 0x3f, 0x3f, 0x07};
static const uint8_t BTST_4G[] = {0x27, 0x27, 0x28, 0x17};
/* 0x3F = the proven mono 0x1F plus the REG bit (LUTs from registers). The GD
 * demo's 0xBF also sets PSR bit7 -- a RES[1:0] resolution-select bit on this
 * controller -- and produced NO refresh on the E1001 bench panel (2026-07-23);
 * Seeed_GFX, bb_epaper and GxEPD2_4G all drive this glass with 0x3F. */
static const uint8_t PSR_4G[]  = {0x3f};
static const uint8_t PLL_4G[]  = {0x06};        /* 50 Hz */
static const uint8_t PWS_4G[]  = {0x88};        /* 0xE3 power saving */
static const uint8_t LPD_4G[]  = {0x00};        /* 0x52 */

/* CDI (0x50) byte 0 at INIT time: VBD[7:6] | DDX[5:4] | CDI[3:0].
 *
 * The border is no longer handled here. It used to be: R25 (LUTBD) got a copy
 * of one of the pixel LUTs and VBD was left at 00 so the border followed it,
 * which drove the border with a full waveform on every refresh and settled it
 * dark. Seeed never writes R25 at all -- they set VBD at REFRESH time instead,
 * in the window preamble (see gray_frame_setup). So this is init-time only and
 * the refresh overwrites it. */
#ifndef GRAY4_CDI_BYTE0
#define GRAY4_CDI_BYTE0 0x10
#endif

/* VCOM DC (command 0x82). Seeed sends 0x12 on this path and always has; the
 * -1 "leave the factory OTP value alone" escape stays available to the tuner
 * build, because a panel that forms the image correctly and then cannot hold it
 * is the one symptom drive time cannot explain and VCOM can. */
#ifndef GRAY4_VDCS
#define GRAY4_VDCS 0x12
#endif

/* Live tuning values. Constant in a normal build (the compiler folds these to
 * the macros above); the EPD_GRAY_TUNER build lets the on-panel sweep change
 * them between refreshes so a whole parameter matrix can be judged on the glass
 * without a reflash per combination. */
static int     s_gray_gain = GRAY4_LUT_GAIN_PCT;
static uint8_t s_gray_cdi0 = GRAY4_CDI_BYTE0;
/* CDI byte 0 in force during the REFRESH, written by the per-frame window
 * preamble. This, not s_gray_cdi0, is the byte whose VBD field decides the
 * border, because the preamble runs after init. Seeed's value; the tuner
 * sweeps it (epd_gray_tune sets both, so a swept border byte is the one
 * actually applied rather than one init quietly overwrites). */
static uint8_t s_gray_frame_cdi0 = 0xa9;
/* Which LUT is uploaded to R25 (border): 0 = none, 1 = WW, 2 = BB. Now 0 --
 * matching Seeed, which leaves R25 alone entirely. Kept as a knob only so the
 * tuner build can still put it back and demonstrate the difference. */
static int     s_gray_border_lut = 0;
/* Runtime VCOM override for the sweep build: -1 keeps the compile-time value
 * (or the factory OTP value when that is -1 too). */
static int     s_gray_vdcs = -1;

void epd_gray_tune(int gain_pct, unsigned cdi_byte0, int border_lut)
{
    s_gray_gain = gain_pct;
    s_gray_cdi0 = (uint8_t)cdi_byte0;
    s_gray_frame_cdi0 = (uint8_t)cdi_byte0;
    s_gray_border_lut = border_lut;
}

void epd_gray_set_vcom(int vdcs)
{
    s_gray_vdcs = vdcs;
}

/* Plane encoding. The two waveform paths need DIFFERENT tables, and using one
 * for the other corrupts the image: feed the register-LUT path the OTP encoding
 * and pure white and pure black come out swapped while the mid-greys pass
 * through untouched, which on a dithered photo reads as a negative. That
 * happened once already, so the two tables live here side by side.
 *
 * The waveform is now chosen at RUNTIME (see gray_probe_waveform), so the
 * encoding has to be too. The two paths genuinely differ:
 *
 * Built-in OTP waveform -- mapped EMPIRICALLY on a production E1001 (bench
 * 2026-07-23) and corroborated by Seeed's own GxEPD2_reTerminal_E1001_Gray4
 * example, which inverts the level (gray = 3 - g) before splitting it:
 *
 *        white  light-gray  dark-gray  black         (g = 3    2    1    0)
 *   0x10:  0        1           0        1           (bit = (g & 1) ^ 1)
 *   0x13:  0        0           1        1           (bit = ((g >> 1) & 1) ^ 1)
 *
 * Register LUTs -- Seeed_GFX's EPD_PUSH_NEW_GRAY_COLORS, which pairs with the
 * LUT tables uploaded below. It is self-consistent with the LUT names, and that
 * is the check that matters: the (old,new) bit pair selects the LUT, so white
 * must land on WW and black on KK or the waveform drives the wrong transition.
 *
 *        white  light-gray  dark-gray  black         (g = 3    2    1    0)
 *   0x10:  1        0           1        0           (bit = g & 1)
 *   0x13:  1        1           0        0           (bit = (g >> 1) & 1)
 *   LUT:   WW       KW          WK       KK
 *
 * The register-LUT row replaces a table derived on hardware against the
 * GoodDisplay LUTs. Those LUTs are gone, and an encoding is only meaningful
 * next to the waveform it was derived with, so it went with them. */
static inline uint8_t gray_plane_bit(uint8_t g, int plane)
{
    if (s_gray_use_otp)
        return (plane == 1) ? (uint8_t)((g & 1) ^ 1)
                            : (uint8_t)(((g >> 1) & 1) ^ 1);
    return (plane == 1) ? (uint8_t)(g & 1)
                        : (uint8_t)((g >> 1) & 1);
}

#define GRAY_P1_BIT(g) gray_plane_bit((uint8_t)(g), 1)   /* DTM1 (0x10) */
#define GRAY_P2_BIT(g) gray_plane_bit((uint8_t)(g), 2)   /* DTM2 (0x13) */

/* Stream one 1bpp plane derived from the 2bpp frame: plane bit =
 * 8 px/byte MSB-first, row by row. plane is 1 (DTM1) or 2 (DTM2) and the bit
 * comes from the macros above, NOT an open-coded shift -- hand-inlining it here
 * is exactly what let this path drift from mono_clear() and the test pattern,
 * which used the macros and so stayed correct while the image did not. */
static void gray_send_plane(uint8_t dtm_cmd, const uint8_t *image, int plane)
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
                    o = (uint8_t)((o << 1) | (gray_plane_bit(g, plane) & 1));
                }
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Preamble every 4-gray frame needs before its planes go down.
 *
 * On the register-LUT path this is Seeed's EPD_SET_WINDOW, which they issue
 * unconditionally at the top of every update: CDI is rewritten to 0xA9, then
 * partial-in (0x91) and a partial window (0x90) covering the whole panel.
 *
 * Two things come out of that, and the first is the one that matters. CDI byte
 * 0 carries VBD, so the border setting that is actually in force during the
 * refresh is 0xA9's VBD=10 -- a fixed level -- not whatever init left behind.
 * That is how the stock firmware gets a clean border without ever writing R25,
 * and it is the answer to the black frame this panel has been growing on every
 * refresh. The border was being driven by an uploaded waveform because we put
 * one in R25 and left VBD following it.
 *
 * The window itself is a no-op geometrically (0,0)-(799,479), but the
 * controller is in partial mode for the transfer, which is the mode Seeed's
 * LUTs were characterised in. The trailing 0x01 is theirs.
 *
 * The built-in OTP path is left alone: it is field-verified as it stands, and
 * Seeed's OTP branch does not share this preamble. */
static void gray_frame_setup(void)
{
    if (s_gray_use_otp) return;

    const uint8_t CDI_FRAME[] = {s_gray_frame_cdi0, 0x07};
    static const uint8_t PTL_FULL[]  = {
        0x00, 0x00,                                   /* x start = 0     */
        (EPD_WIDTH - 1) >> 8, (EPD_WIDTH - 1) & 0xff, /* x end   = 799   */
        0x00, 0x00,                                   /* y start = 0     */
        (EPD_HEIGHT - 1) >> 8, (EPD_HEIGHT - 1) & 0xff,
        0x01,
    };
    cmd_data(CDI, CDI_FRAME, sizeof CDI_FRAME);
    cmd_data(0x91, NULL, 0);                /* partial in */
    cmd_data(0x90, PTL_FULL, sizeof PTL_FULL);
}

/* Emit `rows` solid plane rows (caller has sent the DTM command, CS low). */
static void gray_send_solid_rows(uint8_t fill, int rows)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, fill, sizeof row);
    for (int y = 0; y < rows; y++) send_data(row, sizeof row);
}
#endif /* EPD_GRAY4 */

#ifdef EPD_BWR
/* ------------------------------------------------------------------ */
/* Black/white/RED tri-color (EPD_BWR builds; kind xiao_epaper_75_bwr;
 * DKE DEPG0750RW / GoodDisplay GDEW075Z08-class glass on the UC8179).
 *
 * PSR 0x0F selects the built-in BWR OTP waveform (no register LUTs). Two
 * planes per frame: DTM1 (0x10) = B/W, bit 1 = white; DTM2 (0x13) = red,
 * bit 1 = red ink, red overrides B/W. Corroborated by GxEPD2 750c_Z08, the
 * GoodDisplay GDEY075Z08 demo, bb_epaper EP75R and a DEPG0750RW driver.
 * Full refresh is ~17 s of visible flashing -- normal for this glass.
 *
 * Wire format in: 2bpp packed, 4 px/byte MSB-first, 0b00 = black,
 * 0b01 = white, 0b10 = red, 0b11 reserved (rendered white). The per-plane
 * output bit for value g is (map >> g) & 1: */
#define BWR_KW_MAP   0x0E   /* black->0, white->1, red->1 (white under red), 3->1 */
#define BWR_RED_MAP  0x04   /* red->1, everything else -> 0 */
static const uint8_t PSR_BWR[] = {0x0f};        /* KWR mode, OTP LUTs */
static const uint8_t CDI_BWR[] = {0x11, 0x07};
static const uint8_t BTST_BWR[] = {0x17, 0x17, 0x28, 0x17};

/* Stream one 1bpp plane from the 2bpp frame via the value->bit map. */
static void bwr_send_plane(uint8_t dtm_cmd, const uint8_t *image, uint8_t map)
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
                    o = (uint8_t)((o << 1) | ((map >> g) & 1));
                }
            }
            row[b] = o;
        }
        send_data(row, sizeof row);
    }
    gpio_set_level(EPD_PIN_CS, 1);
}

/* Emit `rows` solid plane rows (caller has sent the DTM command, CS low). */
static void bwr_send_solid_rows(uint8_t fill, int rows)
{
    uint8_t row[EPD_WIDTH / 8];
    memset(row, fill, sizeof row);
    for (int y = 0; y < rows; y++) send_data(row, sizeof row);
}
#endif /* EPD_BWR */

static void mono_init(void)
{
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 1);   /* EN: enable panel power */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif
    hw_reset();

#if defined(EPD_BWR)
    wait_idle();                               /* BUSY high before first cmd */
    cmd_data(PWR,  PWR_V,    sizeof PWR_V);
    cmd_data(0x06, BTST_BWR, sizeof BTST_BWR); /* booster ("enhanced drive") */
    cmd_data(PON,  NULL, 0);
    wait_idle();
    cmd_data(PSR,  PSR_BWR,  sizeof PSR_BWR);
    cmd_data(TRES, TRES_V,   sizeof TRES_V);
    cmd_data(0x15, DSPI_V,   sizeof DSPI_V);
    cmd_data(CDI,  CDI_BWR,  sizeof CDI_BWR);
    cmd_data(TCON, TCON_V,   sizeof TCON_V);
    ESP_LOGI(TAG, "init complete (BWR OTP waveform; full refresh ~17 s)");
#elif defined(EPD_GRAY4)
    if (s_gray_use_otp) {
        /* Built-in OTP 4-gray waveform (bb_epaper epd75_gray_init; TRMNL "a"
         * profile; Seeed's UC8179_INIT_GRAY_OTP). Newer glass carries the table
         * already burned in, selected by fixing the temperature-sensor reading
         * (CCSET TSFIX + TSSET 0x5F). No register LUTs at all -- PSR stays 0x1F
         * exactly like the proven mono init. Verified on a production E1001
         * (bench 2026-07-23), so it is left byte-for-byte as it was even where
         * Seeed's OTP branch differs in ordering. */
        static const uint8_t CDI_G2[]  = {0x90, 0x07};
        static const uint8_t BTST_G2[] = {0x27, 0x27, 0x18, 0x17};
        static const uint8_t CCSET_V[] = {0x02};   /* TSFIX: use TSSET value */
        static const uint8_t TSSET_V[] = {0x5f};   /* picks the OTP 4-gray LUT */

        cmd_data(PSR,  PSR_V,  sizeof PSR_V);      /* 0x1F: KW, OTP LUTs */
        cmd_data(CDI,  CDI_G2, sizeof CDI_G2);
        cmd_data(PON,  NULL, 0);
        wait_idle();
        cmd_data(0x06, BTST_G2, sizeof BTST_G2);
        cmd_data(0xe0, CCSET_V, sizeof CCSET_V);
        cmd_data(0xe5, TSSET_V, sizeof TSSET_V);
        cmd_data(TRES, TRES_V,  sizeof TRES_V);    /* proven on this glass */
        cmd_data(0x15, DSPI_V,  sizeof DSPI_V);
        ESP_LOGI(TAG, "init complete (4-gray built-in OTP waveform)");
    } else {
        /* Register LUTs, for glass with no built-in table. This is Seeed's
         * EPD_INIT_GRAY register-LUT branch in order.
         *
         * The ordering is not incidental. Everything that configures the rails
         * -- PWR, the PLL, VCOM DC and the booster -- goes down BEFORE power-on,
         * because that is when the DC-DC converter starts and latches them. The
         * old sequence set PSR, the PLL, VCOM and CDI after PON, which asked the
         * converter to come up on one set of values and then changed them under
         * it.
         *
         * 0x60 (TCON) and 0x15 (dual-SPI) are deliberately absent: Seeed sends
         * neither on this path, and TCON in particular sets source-to-gate
         * timing that the uploaded waveform assumes the default of. */
        const int vd = (s_gray_vdcs >= 0) ? s_gray_vdcs : GRAY4_VDCS;
        const uint8_t cdi_4g[2] = { s_gray_cdi0, 0x07 };
        const int lut_gain = s_gray_gain;

        cmd_data(PWR,  PWR_4G,  sizeof PWR_4G);
        cmd_data(0x30, PLL_4G,  sizeof PLL_4G);
        if (vd >= 0) { const uint8_t v = (uint8_t)vd; cmd_data(0x82, &v, 1); }
        cmd_data(0x06, BTST_4G, sizeof BTST_4G);   /* booster soft start */
        cmd_data(PON,  NULL, 0);
        wait_idle();
        cmd_data(PSR,  PSR_4G,  sizeof PSR_4G);
        cmd_data(0xe3, PWS_4G,  sizeof PWS_4G);
        cmd_data(CDI,  cdi_4g,  sizeof cdi_4g);
        cmd_data(0x52, LPD_4G,  sizeof LPD_4G);
        cmd_data(TRES, TRES_V,  sizeof TRES_V);

        gray_send_lut(0x20, LUT_VCOM_4G, lut_gain);
        gray_send_lut(0x21, LUT_WW_4G,   lut_gain);
        gray_send_lut(0x22, LUT_BW_4G,   lut_gain);
        gray_send_lut(0x23, LUT_WB_4G,   lut_gain);
        gray_send_lut(0x24, LUT_BB_4G,   lut_gain);
        /* R25 (LUTBD) stays unwritten unless the tuner build asks for it; the
         * border is set by VBD in the per-frame window preamble instead. */
        if (s_gray_border_lut == 1)      gray_send_lut(0x25, LUT_WW_4G, lut_gain);
        else if (s_gray_border_lut == 2) gray_send_lut(0x25, LUT_BB_4G, lut_gain);
        ESP_LOGI(TAG, "init complete (4-gray register LUTs, gain %d%%)",
                 lut_gain);
    }
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
#ifdef EPD_GRAY4
    /* bb_epaper sends DRF with one 0x00 parameter byte in 4-gray mode, and the
     * OTP path is field-verified that way, so it keeps it. Seeed's register-LUT
     * path sends a bare 0x12; with the panel in partial mode by then, a stray
     * data byte after the refresh opcode is not something to improvise on. */
    if (s_gray_use_otp) {
        static const uint8_t DRF_V[] = {0x00};
        cmd_data(DRF, DRF_V, sizeof DRF_V);
    } else {
        cmd_data(DRF, NULL, 0);
    }
#else
    cmd_data(DRF, NULL, 0);
#endif
    /* Confirm the refresh actually STARTED before waiting for it to finish.
     * Skipping this is what leaves a half-drawn or inverted image: wait_idle()
     * returns immediately while BUSY is still high, POF cuts power, and the
     * waveform never runs to completion. */
    int64_t t0 = esp_timer_get_time();
    if (!wait_busy_asserted())
        ESP_LOGW(TAG, "panel never asserted BUSY %d ms after DRF; "
                      "refresh may not have started",
                 BUSY_ASSERT_TIMEOUT_MS);
    wait_idle();
    cmd_data(POF, NULL, 0);
    wait_idle();
    /* Duration is the tell if this ever regresses: a real full refresh is
     * seconds. Sub-second means the panel was powered down early. */
    ESP_LOGI(TAG, "refresh done (%lld ms)",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void mono_display(const uint8_t *image)
{
#if defined(EPD_BWR)
    bwr_send_plane(0x10, image, BWR_KW_MAP);    /* DTM1: B/W, 1 = white */
    bwr_send_plane(DTM2, image, BWR_RED_MAP);   /* DTM2: red, 1 = red   */
    trigger_refresh();
#elif defined(EPD_GRAY4)
    /* Both planes, derived on the fly from the 2bpp buffer (see table). */
    gray_frame_setup();
    gray_send_plane(0x10, image, 1);   /* DTM1, per GRAY_P1_BIT */
    gray_send_plane(DTM2, image, 2);   /* DTM2, per GRAY_P2_BIT */
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
#if defined(EPD_BWR)
    uint8_t g     = (uint8_t)(color & 0x3);
    uint8_t p_kw  = ((BWR_KW_MAP  >> g) & 1) ? 0xFF : 0x00;
    uint8_t p_red = ((BWR_RED_MAP >> g) & 1) ? 0xFF : 0x00;
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    bwr_send_solid_rows(p_kw, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    bwr_send_solid_rows(p_red, EPD_HEIGHT);
    gpio_set_level(EPD_PIN_CS, 1);
    trigger_refresh();
#elif defined(EPD_GRAY4)
    /* Solid gray level: both planes carry that level's bit pattern. */
    uint8_t g  = (uint8_t)(color & 0x3);
    uint8_t p1 = GRAY_P1_BIT(g) ? 0xFF : 0x00;
    uint8_t p2 = GRAY_P2_BIT(g) ? 0xFF : 0x00;
    gray_frame_setup();
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

#if defined(EPD_BWR)
/* BWR selftest: 4 horizontal bands, black / white / RED / white, top to
 * bottom. The band order on glass confirms the plane maps empirically (the
 * gray bring-up taught us reference tables can lie about this family). */
static void mono_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 4;
    static const uint8_t bands[4] = {0x0, 0x1, 0x2, 0x1};   /* blk wht RED wht */

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    for (int b = 0; b < 4; b++)
        bwr_send_solid_rows(((BWR_KW_MAP >> bands[b]) & 1) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int b = 0; b < 4; b++)
        bwr_send_solid_rows(((BWR_RED_MAP >> bands[b]) & 1) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    trigger_refresh();
}
#elif defined(EPD_GRAY4)
/* Gray-ramp selftest: 4 horizontal bands, black -> dark gray -> light gray ->
 * white, top to bottom. Even spacing between the two grays is the bench
 * tuning target (LUT variants exist if a batch renders too light/dark). */
static void mono_show_color_bars(void)
{
    const int BAND_H = EPD_HEIGHT / 4;   /* 120 rows/band */

    gray_frame_setup();
    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(0x10);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows(GRAY_P1_BIT(g) ? 0xFF : 0x00, BAND_H);
    gpio_set_level(EPD_PIN_CS, 1);

    gpio_set_level(EPD_PIN_CS, 0);
    send_cmd(DTM2);
    for (int g = 0; g < 4; g++)
        gray_send_solid_rows(GRAY_P2_BIT(g) ? 0xFF : 0x00, BAND_H);
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
    gray_frame_setup();
#endif
#if defined(EPD_GRAY4) || defined(EPD_BWR)
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
#if defined(EPD_BWR) || defined(EPD_GRAY4)
    /* Float the border before sleeping (GD/GxEPD2 deep-sleep sequence).
     *
     * Without this the border electrode is left sitting at a DC potential for
     * the whole sleep -- minutes to hours -- and that steady bias darkens it.
     * The 4-gray path was missing it because the step arrived with the BWR
     * bring-up and was guarded to that panel alone.
     *
     * This matches the field report exactly, including the detail the earlier
     * border-LUT theory could not explain: a black frame that "is not there for
     * a brief moment" during a refresh. The refresh drives the border along
     * with everything else, briefly relieving it; then power drops, the bias
     * returns, and it darkens again over the sleep. Plain mono is left alone --
     * it is confirmed good on this glass and there is nothing to fix. */
    static const uint8_t CDI_FLOAT[] = {0xf7};
    cmd_data(CDI, CDI_FLOAT, sizeof CDI_FLOAT);
#endif
    uint8_t magic = 0xA5;
    cmd_data(DSLP, &magic, 1);            /* deep sleep */
#ifdef EPD_PIN_PWR
    gpio_set_level(EPD_PIN_PWR, 0);       /* EN low: cut panel power */
#endif
}

/* ---------- exported vtable ---------- */

const epd_driver_t mono_spi_driver = {
    .info = {
#if defined(EPD_BWR)
        .name      = "BWR 7.5\" (800x480, 2bpp, red)",
        .bpp       = 2,
        .grayscale = false,    /* black/white/red: a palette, not gray levels */
#elif defined(EPD_GRAY4)
        .name      = "Mono 7.5\" (800x480, 4-gray 2bpp)",
        .bpp       = 2,
        .grayscale = true,     /* 4 true gray levels */
#else
        .name      = "Mono 7.5\" (800x480, 1bpp)",
        .bpp       = 1,
        .grayscale = false,    /* no intermediate level exists at 1bpp */
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
