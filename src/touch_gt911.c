/* touch_gt911.c -- minimal Goodix GT911 reader. See touch_gt911.h. */

#include "touch_gt911.h"

#ifdef BOARD_HAS_TOUCH

#include "touch_coords.h"
#include "i2c_bus.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/i2c_master.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"   /* esp_rom_delay_us */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

/* --- board wiring (with safe defaults so the file always compiles) --- */
#ifndef BOARD_TOUCH_I2C_ADDR
#define BOARD_TOUCH_I2C_ADDR   0x5d
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

/* GT911 16-bit registers (big-endian on the wire). */
#define GT_REG_CONFIG_X    0x8048   /* X output max, little-endian u16 */
#define GT_REG_CONFIG_Y    0x804a   /* Y output max, little-endian u16 */
#define GT_REG_PRODUCT_ID  0x8140   /* 4 ASCII bytes, e.g. "911\0"     */
#define GT_REG_STATUS      0x814e   /* bit7 = buffer ready, low nibble = #points */
/* First touch point coordinate block: X low/high, Y low/high, little-endian.
 * Verified on real E1003 hardware to start at 0x8150 (X-low), NOT the 0x8151
 * of the common GT9xx map -- a swipe sweeps the 0x8150 byte smoothly while the
 * would-be track-id at 0x8150 stays put, confirming X-low lives here. */
#define GT_REG_POINT1_XY   0x8150   /* Xl,Xh,Yl,Yh of the first touch point */

#define GT_TIMEOUT_MS      50

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool     s_ready = false;
static uint32_t s_product_id = 0;
static int      s_rmax_x = EPD_WIDTH;    /* GT911 configured output maxima */
static int      s_rmax_y = EPD_HEIGHT;

static esp_err_t gt_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };
    return i2c_master_transmit_receive(s_dev, a, 2, data, len, GT_TIMEOUT_MS);
}

static esp_err_t gt_write_u8(uint16_t reg, uint8_t val)
{
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff), val };
    return i2c_master_transmit(s_dev, b, 3, GT_TIMEOUT_MS);
}

/* Reset + I2C-address-select. The GT911 latches its 7-bit address from the INT
 * level at the RST rising edge: INT low -> 0x5d, INT high -> 0x14. We drive the
 * 0x5d sequence explicitly rather than trust board pulls. Leaves TP_RST high and
 * TP_INT as an input (the controller drives it as the interrupt line). */
static void gt_reset_select_5d(void)
{
    const int rst = BOARD_TOUCH_RST_PIN;
    const int intp = BOARD_TOUCH_INT_PIN;

    gpio_hold_dis(rst);   /* release any latch left from the last deep sleep */

    gpio_set_direction(rst,  GPIO_MODE_OUTPUT);
    gpio_set_direction(intp, GPIO_MODE_OUTPUT);

    gpio_set_level(rst, 0);          /* assert reset (active low) */
    gpio_set_level(intp, 0);
    vTaskDelay(pdMS_TO_TICKS(11));
    gpio_set_level(intp, 0);          /* INT low selects address 0x5d */
    esp_rom_delay_us(120);
    gpio_set_level(rst, 1);          /* release reset; address latched here */
    vTaskDelay(pdMS_TO_TICKS(6));
    gpio_set_level(intp, 0);
    vTaskDelay(pdMS_TO_TICKS(55));

    gpio_set_direction(intp, GPIO_MODE_INPUT);   /* controller drives INT now */
    gpio_set_pull_mode(intp, GPIO_FLOATING);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t touch_init(void)
{
    if (s_ready) return ESP_OK;

    esp_err_t err = i2c_bus_get(BOARD_SHT4X_I2C_PORT, BOARD_SHT4X_I2C_SDA,
                                BOARD_SHT4X_I2C_SCL, &s_bus);
    if (err != ESP_OK) { ESP_LOGW(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }

    if (s_dev == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = BOARD_TOUCH_I2C_ADDR,
            .scl_speed_hz    = BOARD_SHT4X_I2C_HZ,
        };
        err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) { ESP_LOGW(TAG, "add dev: %s", esp_err_to_name(err)); return err; }
    }

    /* The GT911 keeps running across our deep sleep (TP_RST external pull-up), so
     * on a touch wake it is already alive at 0x5d. Try reading the product id
     * WITHOUT the ~120 ms reset first -- that latency is subtracted straight off
     * the wake-to-first-sample window, which is what a quick tap races. Only if it
     * does not answer (cold boot / lost address) do the full reset + select. */
    uint8_t id[4] = {0};
    if (gt_read(GT_REG_PRODUCT_ID, id, sizeof id) != ESP_OK ||
        id[0] != '9' || id[1] != '1' || id[2] != '1') {
        gt_reset_select_5d();
        err = gt_read(GT_REG_PRODUCT_ID, id, sizeof id);
        if (err != ESP_OK) { ESP_LOGW(TAG, "product-id read: %s", esp_err_to_name(err)); return err; }
    }
    s_product_id = (uint32_t)id[0] | ((uint32_t)id[1] << 8) |
                   ((uint32_t)id[2] << 16) | ((uint32_t)id[3] << 24);

    /* Read the configured output maxima; fall back to the frame size. */
    uint8_t mx[2] = {0}, my[2] = {0};
    if (gt_read(GT_REG_CONFIG_X, mx, 2) == ESP_OK) {
        int v = mx[0] | (mx[1] << 8);
        if (v > 0) s_rmax_x = v;
    }
    if (gt_read(GT_REG_CONFIG_Y, my, 2) == ESP_OK) {
        int v = my[0] | (my[1] << 8);
        if (v > 0) s_rmax_y = v;
    }

    gt_write_u8(GT_REG_STATUS, 0);   /* clear any stale buffer flag */
    s_ready = true;
    ESP_LOGI(TAG, "GT911 up: id='%c%c%c' max=%dx%d",
             id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?',
             s_rmax_x, s_rmax_y);
    return ESP_OK;
}

uint32_t touch_product_id(void) { return s_product_id; }

esp_err_t touch_read_raw(int *rx, int *ry, bool *pressed)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    uint8_t status = 0;
    esp_err_t err = gt_read(GT_REG_STATUS, &status, 1);
    if (err != ESP_OK) return err;

    if (!(status & 0x80)) { *pressed = false; return ESP_OK; }   /* no fresh buffer */

    int npoints = status & 0x0f;
    if (npoints > 0) {
        uint8_t p[4] = {0};
        err = gt_read(GT_REG_POINT1_XY, p, sizeof p);
        if (err == ESP_OK) {
            *rx = p[0] | (p[1] << 8);
            *ry = p[2] | (p[3] << 8);
            *pressed = true;
        } else {
            *pressed = false;
        }
    } else {
        *pressed = false;   /* buffer ready, finger lifted */
    }

    gt_write_u8(GT_REG_STATUS, 0);   /* MUST clear so the controller refills */
    return err;
}

void touch_translate_raw(int rx, int ry, int *fx, int *fy)
{
    touch_raw_to_frame(rx, ry, s_rmax_x, s_rmax_y, EPD_WIDTH, EPD_HEIGHT,
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
    return gpio_get_level(BOARD_TOUCH_INT_PIN) == 1;   /* active high */
}

esp_err_t touch_capture_stroke(touch_stroke_t *out,
                               uint32_t first_point_ms, uint32_t cap_ms)
{
    out->valid = false;
    out->x0 = out->y0 = out->x1 = out->y1 = 0;
    out->ms = 0;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    int64_t t_start = esp_timer_get_time();
    int64_t first_deadline = t_start + (int64_t)first_point_ms * 1000;
    int64_t cap_deadline   = t_start + (int64_t)cap_ms * 1000;
    int64_t t_first = 0;

    /* Phase 1: wait for the first readable point (quick-tap race window). */
    while (esp_timer_get_time() < first_deadline) {
        int fx = 0, fy = 0; bool pressed = false;
        if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK && pressed) {
            out->x0 = out->x1 = fx;
            out->y0 = out->y1 = fy;
            out->valid = true;
            t_first = esp_timer_get_time();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    if (!out->valid) return ESP_OK;   /* finger already lifted: quick-tap race */

    /* Phase 2: keep sampling until lift or cap; the END point drives sliders. */
    int misses = 0;
    while (esp_timer_get_time() < cap_deadline) {
        int fx = 0, fy = 0; bool pressed = false;
        if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK) {
            if (pressed) {
                out->x1 = fx; out->y1 = fy;
                misses = 0;
            } else if (++misses >= 3) {
                break;   /* three consecutive empty reads => finger lifted */
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
        ESP_LOGW(TAG, "prepare_sleep: GT911 init failed; no touch wake armed");
        return;
    }
    /* Monitor mode: leave the controller scanning (we never command sleep) so it
     * raises INT on a touch. Clear the buffer so a fresh touch triggers. */
    gt_write_u8(GT_REG_STATUS, 0);

    /* Do NOT latch TP_RST with gpio_deep_sleep_hold_en(): verified on E1003
     * hardware that enabling it breaks the ext1 touch wake (the controller stops
     * asserting INT / the SoC never wakes). TP_RST has an external pull-up on the
     * reTerminal, so it stays high through deep sleep on its own and the GT911
     * keeps scanning -- confirmed by a touch waking the device via ext1. */

    /* TP_INT is ACTIVE-LOW (verified on E1003 hardware: idles high, the GT911
     * pulls it low on a touch -- the "active high" board note was wrong). The
     * wake is armed by the caller as an ext1 ANY_LOW bit shared with the buttons
     * (buttons_arm_ext1_with(TOUCH_INT_WAKE_MASK)); ext0 did not fire on this
     * line, but the button ext1 path wakes reliably on the same hardware. */
}

#endif /* BOARD_HAS_TOUCH */
