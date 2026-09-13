/* m5pm1.c -- M5Stack M5PM1 PMIC, battery voltage. See m5pm1.h. */

#include "m5pm1.h"
#include "app_config.h"   /* pulls board.h -> BOARD_M5PM1_* */

#if defined(BOARD_BATTERY_M5PM1) || defined(BOARD_FRONTLIGHT_M5PM1) || defined(BOARD_LED_M5PM1)

#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#define REG_PWR_CFG   0x06   /* [4] LED_EN level, [3] BOOST, [2] LDO, [1] DCDC, [0] CHG */
#define REG_I2C_CFG   0x09   /* [4] 400 kHz, [3:0] idle-sleep timeout s (0 = never) */
#define REG_GPIO_MODE 0x10   /* [4:0] 1 = output */
#define REG_GPIO_DRV  0x13   /* [4:0] 1 = open-drain */
#define REG_GPIO_PUPD0 0x14  /* 2 bits per GPIO0-3: 00 none 01 up 10 down */
#define REG_GPIO_FUNC0 0x16  /* 2 bits per GPIO0-3: 00 gpio 01 irq 10 wake 11 special */
#define REG_VBAT_L    0x22   /* mV low byte; 0x23 holds the high 4 bits */
#define REG_PWM0_L    0x30   /* duty low byte; 0x31 = [5] POL [4] EN [3:0] duty high */
#define REG_PWM_FREQ_L 0x34  /* Hz, 16-bit little-endian */

#define PM1_LED_CTRL_BIT   (1u << 4)
#define PM1_FRONTLIGHT_GPIO 3
#define PM1_PWM_HZ         5000

#define PM1_TIMEOUT_MS  50

/* A 1S Li-Po the chip is genuinely measuring sits in this window; anything
 * else is a mangled transaction and is rejected rather than published. */
#define PM1_MV_MIN    2000
#define PM1_MV_MAX    5000

static const char *TAG = "m5pm1";

/* Survives deep sleep: one skipped wake must not turn a healthy pack into a
 * flat one. Cleared on a cold boot. */
RTC_DATA_ATTR static uint16_t s_mv;
RTC_DATA_ATTR static bool     s_valid;

static bool s_tried;
static i2c_master_dev_handle_t s_dev;

static bool pm1_open(void)
{
    if (s_dev != NULL) return true;
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_bus_get(BOARD_M5PM1_I2C_PORT, BOARD_M5PM1_I2C_SDA,
                    BOARD_M5PM1_I2C_SCL, &bus) != ESP_OK) {
        return false;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_M5PM1_I2C_ADDR,
        .scl_speed_hz    = BOARD_M5PM1_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev) == ESP_OK;
}

/* The PM1 is a small MCU serving I2C in firmware; M5's own library pads every
 * transaction with 500 us before and after (M5PM1_i2c_compat.h). Without the
 * gap, a burst of read-modify-writes returned garbage on the bench. */
#define PM1_GAP_US 500

static bool pm1_xfer_rd(uint8_t reg, uint8_t *rx, size_t n)
{
    esp_rom_delay_us(PM1_GAP_US);
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, rx, n, PM1_TIMEOUT_MS);
    esp_rom_delay_us(PM1_GAP_US);
    return err == ESP_OK;
}

static bool pm1_xfer_wr(const uint8_t *tx, size_t n)
{
    esp_rom_delay_us(PM1_GAP_US);
    esp_err_t err = i2c_master_transmit(s_dev, tx, n, PM1_TIMEOUT_MS);
    esp_rom_delay_us(PM1_GAP_US);
    return err == ESP_OK;
}

static bool pm1_read_vbat(uint16_t *mv)
{
    uint8_t rx[2];
    if (!pm1_xfer_rd(REG_VBAT_L, rx, sizeof rx)) return false;
    /* Full 16 bits. M5's header calls 0x23 "high 4 bits", but a full cell on
     * USB reads 0x1078 (4216 mV): the high byte carries bit 12 too, and a
     * nibble mask turned every reading above 4096 mV into ~120 mV. */
    *mv = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
    return true;
}

/* The PM1 drops its I2C interface into a sleep after an idle timeout and
 * NACKs the first transaction that follows. M5's reference code wakes it by
 * writing the idle timeout to 0 twice (the first write can be swallowed on
 * the way out of sleep); do the same before retrying. */
static bool pm1_rd8(uint8_t reg, uint8_t *v);
static bool pm1_wr8(uint8_t reg, uint8_t v);

static void pm1_wake(void)
{
    uint8_t tx[2] = { REG_I2C_CFG, 0x00 };
    for (int i = 0; i < 2; i++) {
        (void)pm1_xfer_wr(tx, sizeof tx);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

#ifdef M5PM1_DEBUG_DUMP
/* Bring-up aid: every register the firmware touches or reads, plus the
 * chip identity, once per boot. Enable with -DM5PM1_DEBUG_DUMP. */
static void pm1_debug_dump(const char *when)
{
    static const uint8_t regs[] = { 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
                                    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x48, 0x49, 0x4A,
                                    0x10, 0x12, 0x13, 0x16, 0x22, 0x23, 0x24, 0x25 };
    char line[200]; int n = 0;
    for (size_t i = 0; i < sizeof regs; i++) {
        uint8_t v = 0xEE;
        bool ok = pm1_rd8(regs[i], &v);
        n += snprintf(line + n, sizeof line - (size_t)n, " %02x=%s%02x",
                      regs[i], ok ? "" : "!", v);
        if (n > 150 || i + 1 == sizeof regs) { ESP_LOGW(TAG, "dump %s:%s", when, line); n = 0; }
    }
}
#endif

static void pm1_refresh(void)
{
    if (s_tried) return;
    s_tried = true;

    if (!pm1_open()) {
        ESP_LOGW(TAG, "I2C bus unavailable");
        return;
    }
    {
        uint8_t pc = 0;
        if (pm1_rd8(REG_PWR_CFG, &pc)) ESP_LOGI(TAG, "PWR_CFG 0x%02x", pc);
    }
#ifdef M5PM1_DEBUG_DUMP
    pm1_debug_dump("boot");
    /* Clear the wake-source flags and the three IRQ status registers so the
     * NEXT boot's dump shows why the PMIC brought us up, not history. */
    (void)pm1_wr8(0x05, 0x00);
    (void)pm1_wr8(0x40, 0x00);
    (void)pm1_wr8(0x41, 0x00);
    (void)pm1_wr8(0x42, 0x00);
#endif

    uint16_t mv = 0;
    bool ok = pm1_read_vbat(&mv);
    if (!ok) {
        pm1_wake();
        ok = pm1_read_vbat(&mv);
    }
    if (ok && (mv < PM1_MV_MIN || mv > PM1_MV_MAX)) {
        /* One garbage sample right after other PM1 traffic; re-read once. */
        vTaskDelay(pdMS_TO_TICKS(20));
        ok = pm1_read_vbat(&mv);
    }
    if (!ok) {
        ESP_LOGW(TAG, "VBAT read failed%s", s_valid ? "; using last sample" : "");
        return;
    }
    if (mv < PM1_MV_MIN || mv > PM1_MV_MAX) {
        ESP_LOGW(TAG, "VBAT %u mV out of range; ignored", (unsigned)mv);
        return;
    }
    s_mv = mv;
    s_valid = true;
    ESP_LOGI(TAG, "VBAT %u mV", (unsigned)mv);
}

static bool pm1_rd8(uint8_t reg, uint8_t *v)
{
    return pm1_xfer_rd(reg, v, 1);
}

static bool pm1_wr(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t tx[4] = { reg };
    if (n > 3) return false;
    memcpy(tx + 1, data, n);
    return pm1_xfer_wr(tx, n + 1);
}

static bool pm1_wr8(uint8_t reg, uint8_t v) { return pm1_wr(reg, &v, 1); }

/* Read-modify-write one register, defensively. A PM1 read that returns
 * garbage and is written straight back can clear DCDC_EN / LDO_EN in
 * PWR_CFG, which cuts our own power: the bench unit came back from a
 * power-on reset mid-cycle more than once (2026-09-13) with the LED write at
 * the end of the cycle as the prime suspect. So: two reads must agree, a
 * PWR_CFG value that says our rails are off is rejected as a misread (we are
 * running, so they are on), and the result is read back. */
static bool pm1_rmw(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t a = 0, b = 0xFF;
    if (!pm1_rd8(reg, &a)) {
        pm1_wake();
        if (!pm1_rd8(reg, &a)) return false;
    }
    if (!pm1_rd8(reg, &b) || a != b) {
        ESP_LOGW(TAG, "reg 0x%02x read unstable (%02x/%02x); write skipped", reg, a, b);
        return false;
    }
    if (reg == REG_PWR_CFG && (a & 0x06) != 0x06) {
        ESP_LOGW(TAG, "PWR_CFG reads %02x with DCDC/LDO off while running; write skipped", a);
        return false;
    }
    uint8_t want = (uint8_t)((a & ~mask) | (value & mask));
    if (want == a) return true;
    if (!pm1_wr8(reg, want)) return false;
    uint8_t chk = 0;
    if (pm1_rd8(reg, &chk) && chk != want) {
        ESP_LOGW(TAG, "reg 0x%02x wrote %02x, reads back %02x", reg, want, chk);
    }
    return true;
}

#ifdef BOARD_FRONTLIGHT_M5PM1
static int s_frontlight_pct;

bool m5pm1_frontlight_active(void) { return s_frontlight_pct > 0; }

esp_err_t m5pm1_frontlight_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_frontlight_pct = pct;
    if (!pm1_open()) return ESP_ERR_INVALID_STATE;
#ifdef BOARD_BATTERY_M5PM1
    /* Take this boot's VBAT sample BEFORE writing anything: on the bench
     * (2026-09-12) every VBAT read that followed a PM1 write came back ~120 mV
     * for the rest of the boot, while boots that only read were fine. */
    pm1_refresh();
#endif

    /* Same order as M5's PowerDemo set_frontlight(): function "special"
     * (PWM0) on GPIO3, output, no pull, push-pull, then frequency and duty. */
    /* Exactly what M5GFX's Light_M5PaperMono writes, and nothing more: drive
     * push-pull (0x13 bit3 off), function PWM (0x16 bits 7:6 on), frequency,
     * duty. It leaves GPIO_MODE at its input default; M5's PowerDemo sets
     * output as well, and with output set the bench unit's light stayed dark
     * (2026-09-13), so the direction bit is cleared here on purpose. */
    const uint8_t shift = PM1_FRONTLIGHT_GPIO * 2;
    bool ok = pm1_rmw(REG_GPIO_DRV, (uint8_t)(1u << PM1_FRONTLIGHT_GPIO), 0)
           && pm1_rmw(REG_GPIO_FUNC0, (uint8_t)(0x03 << shift), (uint8_t)(0x03 << shift))
           && pm1_rmw(REG_GPIO_MODE, (uint8_t)(1u << PM1_FRONTLIGHT_GPIO), 0)
           && pm1_rmw(REG_GPIO_PUPD0, (uint8_t)(0x03 << shift), 0);
    const uint8_t freq[2] = { PM1_PWM_HZ & 0xff, PM1_PWM_HZ >> 8 };
    const uint16_t duty12 = (uint16_t)((pct * 0x0FFF) / 100);
    const uint8_t duty[2] = { (uint8_t)(duty12 & 0xff),
                              (uint8_t)(((duty12 >> 8) & 0x0f) | (pct ? 0x10 : 0x00)) };
    /* Only touch the PWM registers when they do not already hold the target:
     * the PMIC keeps them across our restarts, and every write is a chance
     * for a misread to land somewhere else. */
    uint8_t cur[2] = {0}, curf[2] = {0};
    bool same = pm1_xfer_rd(REG_PWM0_L, cur, 2) && pm1_xfer_rd(REG_PWM_FREQ_L, curf, 2) &&
                cur[0] == duty[0] && (cur[1] & 0x3f) == duty[1] &&
                curf[0] == freq[0] && curf[1] == freq[1];
    if (!same) {
        ok = ok && pm1_wr(REG_PWM_FREQ_L, freq, 2);
        ok = ok && pm1_wr(REG_PWM0_L, duty, 2);
    }
    if (!ok) {
        ESP_LOGW(TAG, "frontlight %d%%: PMIC write failed", pct);
        return ESP_FAIL;
    }
    /* The PM1 is a small MCU behind the I2C port; a VBAT read issued straight
     * after this burst came back garbage on the bench (122 mV). Let it
     * settle before anyone else on this boot talks to it. */
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "frontlight %d%%", pct);
    return ESP_OK;
}
#endif

#ifdef BOARD_LED_M5PM1
esp_err_t m5pm1_led_set(bool on)
{
    if (!pm1_open()) return ESP_ERR_INVALID_STATE;
#ifdef BOARD_BATTERY_M5PM1
    pm1_refresh();   /* see m5pm1_frontlight_set */
#endif
    return pm1_rmw(REG_PWR_CFG, PM1_LED_CTRL_BIT, on ? PM1_LED_CTRL_BIT : 0) ? ESP_OK : ESP_FAIL;
}
#endif

#ifdef BOARD_BATTERY_M5PM1
bool m5pm1_battery_available(void)
{
    pm1_refresh();
    return s_valid;
}

int m5pm1_battery_mv(void)
{
    pm1_refresh();
    return s_valid ? (int)s_mv : 0;
}
#endif /* BOARD_BATTERY_M5PM1 */

#endif /* BOARD_BATTERY_M5PM1 */
