/*
 * touch_ft6336.c -- FocalTech FT6336G capacitive touch, behind the same
 * touch_*() API as touch_gt911.c (see touch_gt911.h for the contract).
 *
 * Built only when the board defines BOARD_HAS_TOUCH and BOARD_TOUCH_FT6336
 * (M5Stack PaperMono); touch_gt911.c then compiles out. The two controllers
 * share nothing but the idea: the FT6336 has an 8-bit register map, reports
 * up to two points in panel pixels with no configurable output range, needs
 * no address-select dance, and holds INT low for as long as a finger is down
 * when its interrupt mode is "polling" (register 0xA4 = 0), which is exactly
 * the level the ext1 ANY_LOW wake and touch_int_asserted() want.
 *
 * Register map (FocalTech FT6x36 datasheet, cross-checked with M5GFX's
 * Touch_FT5x06.cpp, which drives this very controller on the PaperMono):
 *   0x00 DEV_MODE      0 = working mode
 *   0x02 TD_STATUS     [3:0] number of touch points
 *   0x03 P1_XH         [7:6] event flag, [3:0] X high nibble
 *   0x04 P1_XL         X low byte
 *   0x05 P1_YH         [7:4] touch id, [3:0] Y high nibble
 *   0x06 P1_YL         Y low byte
 *   0x86 CTRL          1 = drop to monitor mode after 0x87 s idle (default)
 *   0xA3 CHIP_ID       0x64 on an FT6336U/G
 *   0xA4 G_MODE        0 = polling (INT level), 1 = trigger (INT pulse)
 *   0xA5 PWR_MODE      0 active, 1 monitor, 3 hibernate
 *   0xA6 FIRMID        firmware version
 *   0xA8 VENDOR_ID     0x11 FocalTech
 *
 * Board knobs beyond the shared ones in touch_gt911.h:
 *   BOARD_TOUCH_RST_M5IOE1_PIN   TP_RST on the M5IOE1 expander (index)
 *   BOARD_TOUCH_EN_M5IOE1_PIN    TP_VDD_EN on the expander (index, active high)
 * Both stay in whatever state they were left across the MCU's deep sleep,
 * because the expander keeps its registers, so a powered, unreset
 * controller is what a touch wake finds -- the warm path below reads it
 * without a reset so the point that woke us is still there.
 */

#include "touch_gt911.h"

#if defined(BOARD_HAS_TOUCH) && defined(BOARD_TOUCH_FT6336)

#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bus.h"
#include "touch_coords.h"
#if defined(BOARD_TOUCH_RST_M5IOE1_PIN) || defined(BOARD_TOUCH_EN_M5IOE1_PIN)
#include "m5ioe1.h"
#endif

static const char *TAG = "touch_ft6336";

#ifndef BOARD_TOUCH_I2C_HZ
#define BOARD_TOUCH_I2C_HZ     400000
#endif
#ifndef BOARD_TOUCH_I2C_ADDR
#define BOARD_TOUCH_I2C_ADDR   0x38
#endif
#ifndef BOARD_TOUCH_SWAP_XY
#define BOARD_TOUCH_SWAP_XY    0
#endif
#ifndef BOARD_TOUCH_INVERT_X
#define BOARD_TOUCH_INVERT_X   0
#endif
#ifndef BOARD_TOUCH_INVERT_Y
#define BOARD_TOUCH_INVERT_Y   0
#endif
#ifndef BOARD_TOUCH_FRAME_W
#define BOARD_TOUCH_FRAME_W    EPD_WIDTH
#endif
#ifndef BOARD_TOUCH_FRAME_H
#define BOARD_TOUCH_FRAME_H    EPD_HEIGHT
#endif

#define FT_REG_DEV_MODE   0x00
#define FT_REG_TD_STATUS  0x02
#define FT_REG_P1_XH      0x03
#define FT_REG_CTRL       0x86
#define FT_REG_CHIP_ID    0xA3
#define FT_REG_G_MODE     0xA4
#define FT_REG_PWR_MODE   0xA5
#define FT_REG_FIRMID     0xA6
#define FT_REG_VENDOR_ID  0xA8

#define FT_TIMEOUT_MS     50
#define FT_MAX_POINTS     2

static i2c_master_dev_handle_t s_dev;
static bool     s_ready;
static uint32_t s_product_id;

static esp_err_t ft_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, FT_TIMEOUT_MS);
}

static esp_err_t ft_write(uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg, val };
    return i2c_master_transmit(s_dev, tx, sizeof tx, FT_TIMEOUT_MS);
}

/* Digitiser rail, if the board gates it (PaperMono: TP_VDD_EN on the
 * expander). Idempotent per boot; the expander keeps it high through sleep. */
static void touch_power_on(void)
{
#ifdef BOARD_TOUCH_EN_M5IOE1_PIN
    static bool s_powered;
    if (s_powered) return;
    if (m5ioe1_set_output(BOARD_TOUCH_EN_M5IOE1_PIN, 1) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_powered = true;
#endif
}

/* Hardware reset through the expander. The FT6336 wants RST low for at least
 * 1 ms and is answering I2C some 100-200 ms after release; M5GFX pulses 1 ms
 * and trusts retries, this waits the datasheet's worst case instead because
 * the cold path only runs when the warm read failed. */
static void ft_hw_reset(void)
{
#ifdef BOARD_TOUCH_RST_M5IOE1_PIN
    touch_power_on();
    m5ioe1_set_output(BOARD_TOUCH_RST_M5IOE1_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    m5ioe1_set_output(BOARD_TOUCH_RST_M5IOE1_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
}

static bool ft_probe(uint8_t *chip, uint8_t *vendor, uint8_t *fw)
{
    if (ft_read(FT_REG_CHIP_ID, chip, 1) != ESP_OK) return false;
    if (ft_read(FT_REG_VENDOR_ID, vendor, 1) != ESP_OK) return false;
    if (ft_read(FT_REG_FIRMID, fw, 1) != ESP_OK) return false;
    /* A controller in reset or unpowered reads back 0x00 or 0xFF on every
     * register; a live one has a vendor id. */
    return *vendor != 0x00 && *vendor != 0xFF;
}

esp_err_t touch_init(void)
{
    if (s_ready) return ESP_OK;

    touch_power_on();

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_bus_get(BOARD_TOUCH_I2C_PORT, BOARD_TOUCH_I2C_SDA,
                                BOARD_TOUCH_I2C_SCL, &bus);
    if (err != ESP_OK) { ESP_LOGW(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }
    if (s_dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BOARD_TOUCH_I2C_ADDR,
            .scl_speed_hz    = BOARD_TOUCH_I2C_HZ,
        };
        err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
        if (err != ESP_OK) { ESP_LOGW(TAG, "add dev 0x%02x: %s", BOARD_TOUCH_I2C_ADDR, esp_err_to_name(err)); return err; }
    }

    /* Warm path first: across our deep sleep the controller stayed powered
     * and unreset, so on a touch wake it is already answering and still
     * holds the point. A reset here would throw that point away. */
    uint8_t chip = 0, vendor = 0, fw = 0;
    bool alive = false;
    for (int i = 0; i < 3 && !alive; i++) {
        alive = ft_probe(&chip, &vendor, &fw);
        if (!alive) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!alive) {
        ft_hw_reset();
        for (int i = 0; i < 10 && !alive; i++) {
            alive = ft_probe(&chip, &vendor, &fw);
            if (!alive) vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
    if (!alive) {
        ESP_LOGW(TAG, "FT6336 not answering at 0x%02x", BOARD_TOUCH_I2C_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    s_product_id = (uint32_t)chip | ((uint32_t)vendor << 8) | ((uint32_t)fw << 16);

    /* Working mode, INT as a level ("polling" mode: low while touched), and
     * the controller's own idle drop to monitor mode left on so a sleeping
     * panel is not paying for active-mode scanning. */
    ft_write(FT_REG_DEV_MODE, 0x00);
    ft_write(FT_REG_G_MODE, 0x00);
    ft_write(FT_REG_CTRL, 0x01);

    /* TP_INT: plain input with a pull-up. The FT6336 drives it open-drain
     * low on a touch, so it idles high with the pull and reads the line
     * correctly whichever path brought us here. */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    s_ready = true;
    ESP_LOGI(TAG, "FT6336 up: chip=0x%02x vendor=0x%02x fw=0x%02x int=%d",
             chip, vendor, fw, gpio_get_level(BOARD_TOUCH_INT_PIN));
    return ESP_OK;
}

uint32_t touch_product_id(void) { return s_product_id; }

esp_err_t touch_read_raw(int *rx, int *ry, bool *pressed)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t st = 0;
    esp_err_t err = ft_read(FT_REG_TD_STATUS, &st, 1);
    if (err != ESP_OK) return err;

    int n = st & 0x0f;
    if (n == 0 || n > FT_MAX_POINTS) { *pressed = false; return ESP_OK; }   /* 0xF = garbage */

    uint8_t p[4] = {0};
    err = ft_read(FT_REG_P1_XH, p, sizeof p);
    if (err != ESP_OK) { *pressed = false; return err; }

    /* Event flag 1 = "lift up": the last report of a finger leaving. Treat it
     * as released so a stroke ends on the lift, not one poll later. */
    if (((p[0] >> 6) & 0x3) == 1) { *pressed = false; return ESP_OK; }

    *rx = ((p[0] & 0x0f) << 8) | p[1];
    *ry = ((p[2] & 0x0f) << 8) | p[3];
    *pressed = true;
    return ESP_OK;
}

void touch_translate_raw(int rx, int ry, int *fx, int *fy)
{
    /* The FT6336 reports in panel pixels already (0..479 x 0..799 on the
     * PaperMono, M5GFX's x_max/y_max), so the raw range IS the frame range and
     * the shared translation only applies the orientation flags and clamps. */
    touch_raw_to_frame(rx, ry, BOARD_TOUCH_FRAME_W - 1, BOARD_TOUCH_FRAME_H - 1,
                       BOARD_TOUCH_FRAME_W, BOARD_TOUCH_FRAME_H,
                       BOARD_TOUCH_SWAP_XY, BOARD_TOUCH_INVERT_X,
                       BOARD_TOUCH_INVERT_Y, fx, fy);
}

esp_err_t touch_read_frame(int *fx, int *fy, bool *pressed)
{
    int rx = 0, ry = 0;
    esp_err_t err = touch_read_raw(&rx, &ry, pressed);
    if (err == ESP_OK && *pressed) touch_translate_raw(rx, ry, fx, fy);
    return err;
}

bool touch_int_asserted(void)
{
    return gpio_get_level(BOARD_TOUCH_INT_PIN) == 0;   /* active low */
}

esp_err_t touch_capture_stroke(touch_stroke_t *out,
                               uint32_t first_point_ms, uint32_t cap_ms)
{
    return touch_capture_stroke_cb(out, first_point_ms, cap_ms, NULL, NULL);
}

/* Identical to the GT911 driver's loop: the stroke semantics (first point,
 * last point, duration, three empty reads = lift) are the server contract,
 * not a controller property. */
esp_err_t touch_capture_stroke_cb(touch_stroke_t *out,
                                  uint32_t first_point_ms, uint32_t cap_ms,
                                  touch_sample_cb_t cb, void *ctx)
{
    out->valid = false;
    out->x0 = out->y0 = out->x1 = out->y1 = 0;
    out->ms = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    int64_t t_start = esp_timer_get_time();
    int64_t first_deadline = t_start + (int64_t)first_point_ms * 1000;
    int64_t cap_deadline   = t_start + (int64_t)cap_ms * 1000;
    int64_t t_first = 0;

    while (esp_timer_get_time() < first_deadline) {
        int fx = 0, fy = 0; bool pressed = false;
        if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK && pressed) {
            out->x0 = out->x1 = fx;
            out->y0 = out->y1 = fy;
            out->valid = true;
            t_first = esp_timer_get_time();
            if (cb) cb(fx, fy, ctx);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    if (!out->valid) return ESP_OK;

    int misses = 0;
    while (esp_timer_get_time() < cap_deadline) {
        int fx = 0, fy = 0; bool pressed = false;
        if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK) {
            if (pressed) {
                out->x1 = fx; out->y1 = fy;
                misses = 0;
                if (cb) cb(fx, fy, ctx);
            } else if (++misses >= 3) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    out->ms = (uint32_t)((esp_timer_get_time() - t_first) / 1000);
    return ESP_OK;
}

void touch_prepare_sleep(void)
{
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "prepare_sleep: FT6336 init failed; no touch wake armed");
        return;
    }
    /* Nothing to latch: the rail and reset live on the expander, which keeps
     * them across the MCU's sleep. Leave the controller in its own idle
     * (monitor) mode; it pulls INT low on the next touch and the caller folds
     * that line into the button ext1 ANY_LOW mask. */
    ft_write(FT_REG_CTRL, 0x01);
}

uint64_t touch_sleep_wake_mask(void) { return TOUCH_INT_WAKE_MASK; }

bool touch_woke_by_gesture(void) { return false; }

#endif /* BOARD_HAS_TOUCH && BOARD_TOUCH_FT6336 */
