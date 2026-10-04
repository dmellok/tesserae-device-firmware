/* touch_gt911.c -- minimal Goodix GT911 reader. See touch_gt911.h. */

#include "touch_gt911.h"

/* A board with a FocalTech controller (BOARD_TOUCH_FT6336) takes
 * touch_ft6336.c instead; the API in touch_gt911.h is shared. */
#if defined(BOARD_HAS_TOUCH) && !defined(BOARD_TOUCH_FT6336)

#include "touch_coords.h"
#include "touch_wakestub.h"   /* g_touch_wake_capture.gesture_armed */
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
/* The touch bus. Boards whose GT911 shares the sensor bus (E1003) leave these
 * undefined and inherit the SHT4x wiring; a board with a dedicated digitiser
 * bus (reTerminal Sticky: SDA3/SCL2 on port 1) names its own. */
#ifndef BOARD_TOUCH_I2C_PORT
#define BOARD_TOUCH_I2C_PORT   BOARD_SHT4X_I2C_PORT
#endif
#ifndef BOARD_TOUCH_I2C_SDA
#define BOARD_TOUCH_I2C_SDA    BOARD_SHT4X_I2C_SDA
#endif
#ifndef BOARD_TOUCH_I2C_SCL
#define BOARD_TOUCH_I2C_SCL    BOARD_SHT4X_I2C_SCL
#endif
#ifndef BOARD_TOUCH_I2C_HZ
#define BOARD_TOUCH_I2C_HZ     BOARD_SHT4X_I2C_HZ
#endif
#ifndef BOARD_TOUCH_I2C_ADDR
#define BOARD_TOUCH_I2C_ADDR   0x5d
#endif
/* BOARD_TOUCH_HOLD_RST: latch TP_RST high through deep sleep. Off by default
 * (the E1003 wakes only with it unheld; its line has an external pull-up).
 * A board with no pull-up on TP_RST, such as the Sticky, sets it. */
#ifndef BOARD_TOUCH_HOLD_RST
#define BOARD_TOUCH_HOLD_RST   0
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
/* Output coordinate space for translated touches. The server hit-tests taps
 * in COMPOSITION space; on most boards that equals the panel geometry
 * (EPD_WIDTH x EPD_HEIGHT) so these defaults are right. A board whose server
 * renderer rotates the composition (M5Paper: esp32_gray_bin composes 540x960
 * and rotates to a 960x540 panel) sets these to the composition dims so the
 * reported coordinates land back in the space the regions live in. */
#ifndef BOARD_TOUCH_FRAME_W
#define BOARD_TOUCH_FRAME_W    EPD_WIDTH
#endif
#ifndef BOARD_TOUCH_FRAME_H
#define BOARD_TOUCH_FRAME_H    EPD_HEIGHT
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
/* Command registers (Goodix GT911 programming guide; the values below are the
 * ones Seeed's SenseCraft HMI firmware writes, lib/GT911/GT911.h + .cpp:
 * enterGestureMode() = 0x8046 <- 0x08, 1 ms, 0x8040 <- 0x08, 10 ms). 0x8046 is
 * the "command check" byte the controller compares against 0x8040 before it
 * accepts a mode change; 0x814b is where gesture mode reports the gesture id
 * instead of a point at 0x8150. */
#define GT_REG_COMMAND     0x8040   /* 0x00 = coordinate mode, 0x05 = sleep, 0x08 = gesture */
#define GT_REG_COMMAND_CHK 0x8046   /* write the same value here first */
#define GT_REG_GESTURE_ID  0x814b   /* gesture-mode result byte, 0 = none */
#define GT_CMD_GESTURE     0x08
#define GT_CMD_SLEEP       0x05   /* stop scanning until reset / host raises INT */

#define GT_TIMEOUT_MS      50

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool     s_ready = false;
static uint32_t s_product_id = 0;
/* GT911 configured output maxima, in the DIGITISER's own space: overwritten by
 * the config-register read in touch_init(), so these are only the fallback if
 * that read fails. The panel dims are the right guess for that (the digitiser
 * is laid out over the glass), not BOARD_TOUCH_FRAME_W/H, which is the OUTPUT
 * space touch_translate_raw() scales into. */
static int      s_rmax_x = EPD_WIDTH;
static int      s_rmax_y = EPD_HEIGHT;

/* Gesture-mode sleep (server config touch_wake "gesture", #327): the RTC flag
 * that says the GT911 was parked in gesture mode lives in the wake-stub stash
 * (g_touch_wake_capture.gesture_armed, touch_wakestub.h) so the stub and this
 * driver read one fact. touch_prepare_sleep(true) sets it; the next boot's
 * touch_init() brings the controller back to coordinate mode before anything
 * reads 0x8150 and clears it. Zero on a cold boot. s_gesture_this_sleep is the
 * awake-side copy for touch_sleep_wake_mask(). */
static bool s_gesture_this_sleep = false;

#ifndef BOARD_TOUCH_EN_PIN
/* Set by touch_park_idle() once the controller took the sleep command, so the
 * next park knows INT is released on the controller side and can check the
 * line for an external pull-up. RTC-retained; cleared on any other path. */
#define PARKED_MAGIC 0x5041524bu   /* 'PARK' */
RTC_DATA_ATTR static uint32_t s_parked;
#endif

/* touch_prepare_sleep() found INT still asserted after clearing the buffer, so
 * folding it into the ANY_LOW wake mask would wake the SoC at once. */
static bool s_int_wake_unsafe = false;

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

/* (Re)bind s_dev to `addr` on the shared bus. Used to probe the GT911's two
 * possible I2C addresses on a board that cannot strap it (no TP_RST). */
static esp_err_t gt_open_at(uint8_t addr)
{
    if (s_dev != NULL) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = BOARD_TOUCH_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_dev);
}

/* True when a product-id read returns "911..." into `id`. */
static bool gt_id_ok(uint8_t id[4])
{
    return gt_read(GT_REG_PRODUCT_ID, id, 4) == ESP_OK &&
           id[0] == '9' && id[1] == '1' && id[2] == '1';
}

/* Digitiser power rail. Some boards gate the GT911 behind a load switch
 * (Sticky: TOUCH_EN on GPIO42, active high). Drive it on before any bus
 * traffic, release a hold left from the last sleep first (a held pad ignores
 * the write), and give the rail a moment to settle. Compiles to nothing when
 * the board leaves BOARD_TOUCH_EN_PIN undefined. */
static void touch_power_on(void)
{
#ifdef BOARD_TOUCH_EN_PIN
    static bool s_powered = false;
    if (s_powered) return;
    /* Configure the drive BEFORE releasing a hold left from the last sleep:
     * the registers take the write while the pad is still latched, so when
     * the hold drops the pad is already driven high and the rail never dips
     * on a touch wake (a dip would reset the GT911 and lose the point). */
    gpio_set_level((gpio_num_t)BOARD_TOUCH_EN_PIN, 1);
    gpio_set_direction((gpio_num_t)BOARD_TOUCH_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_EN_PIN);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_powered = true;
#endif
}

#ifdef BOARD_TOUCH_RST_PIN
/* Reset + I2C-address-select. The GT911 latches its 7-bit address from the INT
 * level at the RST rising edge: INT low -> 0x5d, INT high -> 0x14. We drive the
 * 0x5d sequence explicitly rather than trust board pulls. Leaves TP_RST high and
 * TP_INT as an input (the controller drives it as the interrupt line).
 *
 * Compiled only when the board routes TP_RST to the MCU. A board that does not
 * (M5Paper, PaperS3) relies on the controller being permanently powered and
 * hardware-strapped to its address; touch_init() just retries the warm read. */
static void gt_reset_select_5d(void)
{
    const int rst = BOARD_TOUCH_RST_PIN;
    const int intp = BOARD_TOUCH_INT_PIN;

    gpio_hold_dis(rst);   /* release any latch left from the last deep sleep */
    gpio_hold_dis(intp);  /* ditto: touch_park_idle() latches INT low */
    touch_power_on();     /* no-op unless the digitiser rail is gated */

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
#endif /* BOARD_TOUCH_RST_PIN */

esp_err_t touch_init(void)
{
    if (s_ready) return ESP_OK;

    /* TP_INT may be latched low by a touch_park_idle() from the last sleep (or
     * from before a reset, since a pad hold outlives one). Release it here, up
     * front: a held pad ignores every gpio_config() below, so the warm path --
     * which never runs the reset sequence that does its own release -- would
     * otherwise read the line through a pad still driving it. */
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_INT_PIN);

    touch_power_on();

    esp_err_t err = i2c_bus_get(BOARD_TOUCH_I2C_PORT, BOARD_TOUCH_I2C_SDA,
                                BOARD_TOUCH_I2C_SCL, &s_bus);
    if (err != ESP_OK) { ESP_LOGW(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }

    if (gt_open_at(BOARD_TOUCH_I2C_ADDR) != ESP_OK) {
        ESP_LOGW(TAG, "add dev 0x%02x failed", BOARD_TOUCH_I2C_ADDR);
        return ESP_FAIL;
    }

    /* The GT911 keeps running across our deep sleep, so on a touch wake it is
     * already alive at its address. Try a product-id read WITHOUT the ~120 ms
     * reset first -- that latency is subtracted straight off the wake-to-first-
     * sample window, which is what a quick tap races. */
    uint8_t id[4] = {0};
    bool alive = gt_id_ok(id);
    /* The last sleep parked the controller in gesture mode (touch_wake
     * "gesture"). It still answers its product id there, so the warm path
     * above would happily leave it reporting gesture ids at 0x814b instead of
     * points at 0x8150. Return it to coordinate mode first. With a reset line
     * that is a hardware reset, which is how Seeed's firmware leaves gesture
     * mode (every boot re-runs probe_gt911()'s RST low/high) and how the
     * Goodix reference driver's wakeup path does it; the reset also re-latches
     * the 0x5d address and restores the flash-stored config. Without one, fall
     * back to the soft command (0x8040 <- 0x00, "read coordinate status") and
     * hope; touch_prepare_sleep() refuses gesture mode on such a board, so
     * that arm is only reached after a downgrade or a flag an older build
     * left behind. */
    if (g_touch_wake_capture.gesture_armed == TOUCH_GESTURE_ARMED_MAGIC) {
        g_touch_wake_capture.gesture_armed = 0;
        /* The gesture sleep armed an RTC pull-down on INT for the active-high
         * ext0 wake. In coordinate mode the controller drives INT high when
         * idle, so a pull-down left on would fight it for the whole awake
         * window. Hand the pad back to the digital matrix without it. */
        rtc_gpio_pulldown_dis((gpio_num_t)BOARD_TOUCH_INT_PIN);
        rtc_gpio_deinit((gpio_num_t)BOARD_TOUCH_INT_PIN);
#ifdef BOARD_TOUCH_RST_PIN
        ESP_LOGI(TAG, "leaving gesture mode: hardware reset");
        alive = false;   /* take the reset path below */
#else
        ESP_LOGI(TAG, "leaving gesture mode: 0x8040 <- 0x00");
        gt_write_u8(GT_REG_COMMAND, 0x00);
        vTaskDelay(pdMS_TO_TICKS(10));
        gt_write_u8(GT_REG_GESTURE_ID, 0x00);
#endif
    }
    if (!alive) {
#ifdef BOARD_TOUCH_RST_PIN
        gt_reset_select_5d();
        err = gt_read(GT_REG_PRODUCT_ID, id, sizeof id);
        if (err != ESP_OK) { ESP_LOGW(TAG, "product-id read: %s", esp_err_to_name(err)); return err; }
#else
        /* No MCU reset line: the address latched at power-on cannot be forced
         * and which of the GT911's two it landed on is not knowable here, so
         * probe both -- the configured one a few times, then the alternate
         * (M5GFX does the same alternation for the M5Paper). */
        const uint8_t alt = (BOARD_TOUCH_I2C_ADDR == 0x14) ? 0x5d : 0x14;
        for (int i = 0; i < 4 && !alive; i++) {
            vTaskDelay(pdMS_TO_TICKS(20));
            alive = gt_id_ok(id);
        }
        if (!alive && gt_open_at(alt) == ESP_OK) {
            for (int i = 0; i < 4 && !alive; i++) {
                vTaskDelay(pdMS_TO_TICKS(20));
                alive = gt_id_ok(id);
            }
            if (alive) ESP_LOGI(TAG, "GT911 answered at the alternate 0x%02x", alt);
        }
        if (!alive) {
            ESP_LOGW(TAG, "GT911 not answering at 0x14 or 0x5d (no reset line)");
            return ESP_ERR_NOT_FOUND;
        }
#endif
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

    /* TP_INT must be a plain GPIO input whichever way we got here. The reset
     * path above leaves it that way, but the warm path (controller already
     * answering, no reset) never touched the pad, and after a deep sleep or a
     * cold boot it can sit unconfigured with its input buffer off, so
     * touch_int_asserted() reads a constant instead of the line. Pull-up: the
     * GT911 drives INT actively, so it idles high with or without one -- and a
     * classic-ESP32 input-only pad (GPIO34-39, the M5Paper's INT) has no
     * internal pull to ask for, so requesting one only logs an error. */
    {
#if defined(CONFIG_IDF_TARGET_ESP32) && (BOARD_TOUCH_INT_PIN >= 34)
        const gpio_pullup_t int_pu = GPIO_PULLUP_DISABLE;
#else
        const gpio_pullup_t int_pu = GPIO_PULLUP_ENABLE;
#endif
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << BOARD_TOUCH_INT_PIN,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = int_pu,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
    }
    s_ready = true;
    ESP_LOGI(TAG, "GT911 up: id='%c%c%c' max=%dx%d int=%d",
             id[0] ? id[0] : '?', id[1] ? id[1] : '?', id[2] ? id[2] : '?',
             s_rmax_x, s_rmax_y, gpio_get_level(BOARD_TOUCH_INT_PIN));
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
    touch_raw_to_frame(rx, ry, s_rmax_x, s_rmax_y,
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
    /* ACTIVE-LOW, matching the verified E1003 wiring (idles high, the GT911
     * pulls it low on a touch report -- same polarity the ext1 wake uses). The
     * original "== 1" had this backwards, which inverted the linger loop's gate:
     * it busy-captured while idle and slept 20 ms during actual touches. */
    return gpio_get_level(BOARD_TOUCH_INT_PIN) == 0;
}

esp_err_t touch_capture_stroke(touch_stroke_t *out,
                               uint32_t first_point_ms, uint32_t cap_ms)
{
    return touch_capture_stroke_cb(out, first_point_ms, cap_ms, NULL, NULL);
}

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

    /* Phase 1: wait for the first readable point (quick-tap race window). */
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
    if (!out->valid) return ESP_OK;   /* finger already lifted: quick-tap race */

    /* Phase 2: keep sampling until lift or cap; the END point drives sliders. */
    int misses = 0;
    while (esp_timer_get_time() < cap_deadline) {
        int fx = 0, fy = 0; bool pressed = false;
        if (touch_read_frame(&fx, &fy, &pressed) == ESP_OK) {
            if (pressed) {
                out->x1 = fx; out->y1 = fy;
                misses = 0;
                if (cb) cb(fx, fy, ctx);
            } else if (++misses >= 3) {
                break;   /* three consecutive empty reads => finger lifted */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
    out->ms = (uint32_t)((esp_timer_get_time() - t_first) / 1000);
    return ESP_OK;
}

void touch_prepare_sleep(bool gesture_mode)
{
    s_gesture_this_sleep = false;
    s_int_wake_unsafe    = false;
    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "prepare_sleep: GT911 init failed; no touch wake armed");
        return;
    }
#ifndef BOARD_TOUCH_EN_PIN
    s_parked = 0;   /* touch_init() just woke or reset it; it is scanning now */
#endif
    /* Clear the buffer so a fresh touch triggers. In tap mode the controller
     * is left scanning (monitor mode; we never command sleep) so it raises INT
     * on a touch; gesture mode is commanded further down. */
    gt_write_u8(GT_REG_STATUS, 0);

#ifndef BOARD_TOUCH_RST_PIN
    /* Coming back from gesture mode is a hardware reset in touch_init(); the
     * soft-command fallback there has never been seen to work on glass. A
     * board whose TP_RST is not wired to the MCU (M5Paper, PaperS3) keeps the
     * tap wake whatever the server asked for. */
    if (gesture_mode) {
        ESP_LOGW(TAG, "prepare_sleep: gesture-mode sleep needs TP_RST on this board; waking on tap");
        gesture_mode = false;
    }
#endif
    s_gesture_this_sleep = gesture_mode;

    if (!gesture_mode) {
        /* INT must be idle (high) before it joins the ANY_LOW mask, or the
         * sleep ends the moment it starts. The case that bites is the first
         * touch-on sleep after a touch-off one: touch_init() has just reset a
         * parked controller, and it can still hold INT low for a report or its
         * own start-up when we get here. Give it a short window, re-clearing
         * the buffer, and skip the touch wake for this one sleep rather than
         * take an immediate spurious wake (buttons and the timer still wake
         * it). */
        for (int i = 0; i < 10 && touch_int_asserted(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
            gt_write_u8(GT_REG_STATUS, 0);
        }
        if (touch_int_asserted()) {
            s_int_wake_unsafe = true;
            ESP_LOGW(TAG, "prepare_sleep: INT still asserted after 100 ms; "
                          "touch wake skipped for this sleep");
        }
    }

    /* TP_RST: by default do NOT latch it with gpio_deep_sleep_hold_en().
     * Verified on E1003 hardware that enabling it breaks the ext1 touch wake
     * (the controller stops asserting INT / the SoC never wakes). TP_RST has an
     * external pull-up on the reTerminal, so it stays high through deep sleep
     * on its own and the GT911 keeps scanning -- confirmed by a touch waking
     * the device via ext1. A board without that pull-up opts in with
     * BOARD_TOUCH_HOLD_RST so the line cannot float low and reset the
     * controller mid-sleep. */
#if BOARD_TOUCH_HOLD_RST
    gpio_set_level((gpio_num_t)BOARD_TOUCH_RST_PIN, 1);
    gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST_PIN);
    gpio_deep_sleep_hold_en();
#endif

    /* Gated digitiser rail: hold TOUCH_EN high or the GT911 loses power the
     * moment the pads isolate, and nothing is left to raise INT. This is the
     * standing cost the touch_enabled help text warns about. */
#ifdef BOARD_TOUCH_EN_PIN
    gpio_set_level((gpio_num_t)BOARD_TOUCH_EN_PIN, 1);
    gpio_hold_en((gpio_num_t)BOARD_TOUCH_EN_PIN);
    gpio_deep_sleep_hold_en();
#endif

    if (gesture_mode) {
        /* Gesture-mode sleep (touch_wake "gesture"), as Seeed's SenseCraft HMI firmware does it
         * (Gt911Touch::enableGestureWakeup + GT911::enterGestureMode): a GT911 left
         * in normal scan draws mA-class current for the whole sleep; in gesture
         * mode it idles at a low scan rate and raises INT ACTIVE-HIGH when it
         * recognises a gesture. Sequence, with Seeed's delays:
         *   0x814b <- 0x00   clear a stale gesture id      (GT911::clearGesture)
         *   0x8046 <- 0x08   command check                 (GT911::enterGestureMode)
         *   1 ms
         *   0x8040 <- 0x08   enter gesture mode
         *   10 ms
         * Then TP_INT becomes an RTC input with the pull-DOWN on and the pull-up
         * off, we wait up to 300 ms for it to settle low (the controller drops it
         * once the mode switch takes; if it is still high the sleep will wake at
         * once), and arm ext0 on level 1. The buttons keep their own ext1 mask;
         * touch_sleep_wake_mask() returns 0 in this mode so the caller does not
         * also fold INT into the ANY_LOW mask with a pull-up (that would wake
         * immediately, since INT now idles low). */
        const gpio_num_t intp = (gpio_num_t)BOARD_TOUCH_INT_PIN;
        gt_write_u8(GT_REG_GESTURE_ID, 0x00);
        gt_write_u8(GT_REG_COMMAND_CHK, GT_CMD_GESTURE);
        esp_rom_delay_us(1000);
        esp_err_t err = gt_write_u8(GT_REG_COMMAND, GT_CMD_GESTURE);
        vTaskDelay(pdMS_TO_TICKS(10));
        if (err != ESP_OK) ESP_LOGW(TAG, "gesture-mode command: %s", esp_err_to_name(err));

        rtc_gpio_init(intp);
        rtc_gpio_set_direction(intp, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pullup_dis(intp);
        rtc_gpio_pulldown_en(intp);

        int64_t t0 = esp_timer_get_time();
        while (rtc_gpio_get_level(intp) != 0 &&
               esp_timer_get_time() - t0 < 300 * 1000) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        int lvl = rtc_gpio_get_level(intp);
        ESP_LOGI(TAG, "gesture sleep: INT=%d before sleep (%s), ext0 active-high armed",
                 lvl, lvl == 0 ? "idle" : "STILL ACTIVE, may wake at once");

        err = esp_sleep_enable_ext0_wakeup(intp, 1);
        if (err != ESP_OK) ESP_LOGW(TAG, "ext0 arm: %s", esp_err_to_name(err));
        g_touch_wake_capture.gesture_armed = TOUCH_GESTURE_ARMED_MAGIC;
        return;
    }
    /* Tap mode. TP_INT is ACTIVE-LOW (verified on E1003 hardware: idles high,
     * the GT911 pulls it low on a touch -- the "active high" board note was
     * wrong). The wake is armed by the caller as an ext1 ANY_LOW bit shared
     * with the buttons (buttons_arm_ext1_with(touch_sleep_wake_mask())); ext0
     * did not fire on this line, but the button ext1 path wakes reliably on
     * the same hardware. */
}

void touch_park_idle(void)
{
#ifdef BOARD_TOUCH_EN_PIN
    /* Gated digitiser rail (Sticky): with touch off, touch_power_on() was never
     * called, so the controller has no power to spend. Opening the bus here
     * would RAISE that rail to talk to it -- the exact opposite of the point. */
#else
    /* Permanently powered digitiser (E1003). The controller is scanning right
     * now and will keep scanning through the sleep unless it is told not to.
     *
     * touch_init() takes the warm path when it is still awake and the ~120 ms
     * reset path when the last sleep already parked it (it stops answering its
     * product id in sleep mode, so !alive -> gt_reset_select_5d()); either way
     * we end up talking to a controller in coordinate mode and can re-issue the
     * command. Re-parking every cycle rather than tracking the state in RTC
     * memory keeps this self-healing after any reset, and the reset is the only
     * cost -- a fraction of one percent of a five-minute wake cycle. */
    const gpio_num_t intp = (gpio_num_t)BOARD_TOUCH_INT_PIN;

    /* Self-check for server #327: with the controller asleep (it released INT
     * when the last park took), briefly let go of the line with only a weak
     * internal pull-down on it. A high read means something on the digitiser
     * side pulls INT up, and holding it low below costs that resistor's
     * current all sleep. The main board has no INT pull-up (Seeed schematic
     * sheet 6); the touch FPC is the unknown. Log only: a release that lets
     * the line rise may wake the controller, which the init below handles. */
#if !(defined(CONFIG_IDF_TARGET_ESP32) && (BOARD_TOUCH_INT_PIN >= 34))
    /* (Not on a classic-ESP32 input-only INT pad: it has no internal pulls.) */
    if (s_parked == PARKED_MAGIC) {
        gpio_hold_dis(intp);
        gpio_set_direction(intp, GPIO_MODE_INPUT);
        gpio_set_pull_mode(intp, GPIO_PULLDOWN_ONLY);
        esp_rom_delay_us(2000);
        int lvl = gpio_get_level(intp);
        gpio_set_pull_mode(intp, GPIO_FLOATING);
        ESP_LOGI(TAG, "park_idle: TP_INT with controller asleep reads %d (%s)",
                 lvl, lvl ? "external pull-up present" : "no external pull-up");
    }
#endif
    s_parked = 0;

    if (touch_init() != ESP_OK) {
        ESP_LOGW(TAG, "park_idle: GT911 init failed; left scanning");
        return;
    }
    /* TP_INT low, as an output, BEFORE the command and latched after it. This
     * is Goodix's own enter-sleep sequence, and the reason for it is that the
     * controller leaves sleep on a HIGH on this line: an awake GT911 drives INT
     * push-pull, a sleeping one releases it, and a released line with nothing
     * holding it is free to drift up and wake the chip again. Letting the pad
     * isolate at deep-sleep entry would do exactly that, which would leave this
     * whole function saving nothing while still looking like it worked.
     *
     * If the line turns out to carry an external pull-up after all, driving it
     * low costs that resistor's current for the sleep -- a few hundred uA
     * against the several mA of a scanning digitiser, so the trade is worth
     * taking blind. Released by touch_init() on any path that uses touch
     * again.
     *
     * Our own pulls go off first. touch_init() leaves the pad an input with
     * the internal pull-up on, and a touch-on sleep arms the RTC pull-up for
     * the ext1 wake; the hold below would latch either against the low drive,
     * ~70 uA for the whole sleep (server #327). */
#if !(defined(CONFIG_IDF_TARGET_ESP32) && (BOARD_TOUCH_INT_PIN >= 34))
    gpio_set_pull_mode(intp, GPIO_FLOATING);
    rtc_gpio_pullup_dis(intp);
    rtc_gpio_pulldown_dis(intp);
#endif
    gpio_set_level(intp, 0);
    gpio_set_direction(intp, GPIO_MODE_OUTPUT);

    esp_err_t err = gt_write_u8(GT_REG_COMMAND, GT_CMD_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "park_idle: sleep command: %s", esp_err_to_name(err));
        gpio_set_direction(intp, GPIO_MODE_INPUT);   /* leave the line as found */
        return;
    }
    gpio_hold_en(intp);
    gpio_deep_sleep_hold_en();

    s_parked = PARKED_MAGIC;
    s_ready = false;   /* the controller is asleep; a later read must re-init */
    ESP_LOGI(TAG, "touch disabled: GT911 parked in sleep mode");
#endif
}

uint64_t touch_sleep_wake_mask(void)
{
    if (s_gesture_this_sleep) return 0;   /* ext0 armed inside touch_prepare_sleep() */
    if (s_int_wake_unsafe)    return 0;   /* INT stuck low: see touch_prepare_sleep() */
    return TOUCH_INT_WAKE_MASK;   /* caller folds INT into the ext1 ANY_LOW mask */
}

bool touch_woke_by_gesture(void)
{
    /* Nothing else on these boards arms ext0 (the buttons use ext1 on the S3),
     * so an ext0 wake after a gesture-mode sleep can only be the gesture INT.
     * Read it before touch_init() consumes the flag. */
    return g_touch_wake_capture.gesture_armed == TOUCH_GESTURE_ARMED_MAGIC &&
           esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
}

#endif /* BOARD_HAS_TOUCH */
