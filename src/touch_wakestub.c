/* touch_wakestub.c -- RTC deep-sleep wake stub GT911 capture. See touch_wakestub.h. */

#include "touch_wakestub.h"

/* The stash is always defined so app_main links the same either way. Placed in
 * RTC_DATA so it is zeroed on cold boot and retained across deep sleep. */
#include "esp_attr.h"
RTC_DATA_ATTR touch_wake_capture_t g_touch_wake_capture;

bool touch_wakestub_take(int *rx, int *ry)
{
    if (g_touch_wake_capture.magic != TOUCH_WAKE_MAGIC) return false;
    if (rx) *rx = g_touch_wake_capture.rx;
    if (ry) *ry = g_touch_wake_capture.ry;
    g_touch_wake_capture.magic = 0;   /* consume so it is not replayed */
    return true;
}

#ifdef BOARD_TOUCH_WAKE_STUB

#include "esp_sleep.h"        /* esp_default_wake_deep_sleep, esp_wake_deep_sleep */
#include "esp_rom_sys.h"      /* esp_rom_delay_us (ROM, RTC-safe) */
#include "soc/gpio_reg.h"     /* GPIO_*_W1TS/W1TC_REG, GPIO_IN_REG */
#include "soc/io_mux_reg.h"   /* IO_MUX_GPIOn_REG, PIN_FUNC_GPIO, PIN_* helpers */
#include "soc/usb_serial_jtag_reg.h"   /* USB_SERIAL_JTAG_CONF0: USB pad enable */

/* --- bus wiring (shared with the SHT4x/GT911 I2C bus, port 0) --- */
#define TWS_SDA   BOARD_SHT4X_I2C_SDA
#define TWS_SCL   BOARD_SHT4X_I2C_SCL
#define TWS_ADDR  BOARD_TOUCH_I2C_ADDR   /* 7-bit, 0x5d */

/* GT911 registers (16-bit, big-endian address on the wire). Kept in sync with
 * touch_gt911.c: point-1 block verified at 0x8150 on E1003 hardware. */
#define TWS_REG_STATUS   0x814e
#define TWS_REG_POINT1   0x8150

/* Half an I2C clock period. ~5 us => ~100 kHz, matching the driver's bus speed. */
#define TWS_HALF_US      5
/* Bound on clock-stretch / bit loops so a wedged bus can never hang the boot. */
#define TWS_STRETCH_MAX  200

/* Only pins 0..31 are reachable through the single-word GPIO_* registers used
 * here; both GPIO19 (SDA) and GPIO20 (SCL) qualify on the reTerminal E1003. */
_Static_assert(TWS_SDA < 32 && TWS_SCL < 32,
               "wake-stub bit-bang needs SDA/SCL in the low GPIO bank (0..31)");

/* Map a GPIO number to its IO_MUX register at compile time. Only the pins this
 * stub actually drives need an entry. */
static inline uint32_t RTC_IRAM_ATTR tws_iomux_reg(int pin)
{
    return (pin == TWS_SDA) ? IO_MUX_GPIO19_REG : IO_MUX_GPIO20_REG;
}

/* Open-drain line control: "release" floats the pin (external pull-up drives it
 * high), "drive low" actively pulls it to ground. Never drive a line high. */
static inline void RTC_IRAM_ATTR tws_release(int pin)
{
    REG_WRITE(GPIO_ENABLE_W1TC_REG, 1u << pin);   /* output off -> input, pulled up */
}
static inline void RTC_IRAM_ATTR tws_drive_low(int pin)
{
    REG_WRITE(GPIO_OUT_W1TC_REG, 1u << pin);      /* out latch = 0 */
    REG_WRITE(GPIO_ENABLE_W1TS_REG, 1u << pin);   /* output on -> actively low */
}
static inline int RTC_IRAM_ATTR tws_read(int pin)
{
    return (REG_READ(GPIO_IN_REG) >> pin) & 1;
}

/* Configure one pin as a plain GPIO with input buffer + pull-up enabled. The
 * digital IO_MUX is reset across deep sleep, so this must run every wake. */
static inline void RTC_IRAM_ATTR tws_pin_init(int pin)
{
    uint32_t mux = tws_iomux_reg(pin);
    PIN_INPUT_ENABLE(mux);
    PIN_FUNC_SELECT(mux, PIN_FUNC_GPIO);
    PIN_PULLUP_EN(mux);
    tws_release(pin);                              /* idle high */
}

/* Raise SCL and wait out any clock stretching (bounded). */
static inline void RTC_IRAM_ATTR tws_scl_high(void)
{
    tws_release(TWS_SCL);
    for (int i = 0; i < TWS_STRETCH_MAX && !tws_read(TWS_SCL); i++)
        esp_rom_delay_us(1);
    esp_rom_delay_us(TWS_HALF_US);
}
static inline void RTC_IRAM_ATTR tws_scl_low(void)
{
    tws_drive_low(TWS_SCL);
    esp_rom_delay_us(TWS_HALF_US);
}

static void RTC_IRAM_ATTR tws_start(void)
{
    tws_release(TWS_SDA);
    tws_scl_high();
    tws_drive_low(TWS_SDA);
    esp_rom_delay_us(TWS_HALF_US);
    tws_scl_low();
}

static void RTC_IRAM_ATTR tws_stop(void)
{
    tws_drive_low(TWS_SDA);
    tws_scl_high();
    tws_release(TWS_SDA);
    esp_rom_delay_us(TWS_HALF_US);
}

/* Write one byte, return true if the slave ACKed (SDA low on the 9th clock). */
static bool RTC_IRAM_ATTR tws_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        if (b & 0x80) tws_release(TWS_SDA); else tws_drive_low(TWS_SDA);
        b <<= 1;
        esp_rom_delay_us(TWS_HALF_US);
        tws_scl_high();
        tws_scl_low();
    }
    tws_release(TWS_SDA);        /* let the slave drive ACK */
    tws_scl_high();
    int ack = (tws_read(TWS_SDA) == 0);
    tws_scl_low();
    return ack;
}

/* Read one byte; send ACK (more to come) or NACK (last byte). */
static uint8_t RTC_IRAM_ATTR tws_read_byte(bool ack)
{
    uint8_t v = 0;
    tws_release(TWS_SDA);
    for (int i = 0; i < 8; i++) {
        tws_scl_high();
        v = (uint8_t)((v << 1) | tws_read(TWS_SDA));
        tws_scl_low();
    }
    if (ack) tws_drive_low(TWS_SDA); else tws_release(TWS_SDA);
    esp_rom_delay_us(TWS_HALF_US);
    tws_scl_high();
    tws_scl_low();
    tws_release(TWS_SDA);
    return v;
}

/* Read len bytes from a 16-bit GT911 register into buf. Returns false on any
 * missing ACK (device absent / bus wedged). Bounded; never hangs. */
static bool RTC_IRAM_ATTR tws_read_reg(uint16_t reg, uint8_t *buf, int len)
{
    tws_start();
    if (!tws_write_byte((uint8_t)(TWS_ADDR << 1)))        goto fail;   /* addr + W */
    if (!tws_write_byte((uint8_t)(reg >> 8)))             goto fail;
    if (!tws_write_byte((uint8_t)(reg & 0xff)))           goto fail;
    tws_start();                                                        /* repeated start */
    if (!tws_write_byte((uint8_t)((TWS_ADDR << 1) | 1)))  goto fail;   /* addr + R */
    for (int i = 0; i < len; i++)
        buf[i] = tws_read_byte(i < len - 1);
    tws_stop();
    return true;
fail:
    tws_stop();
    return false;
}

/* The deep-sleep wake stub. Runs from RTC memory ~1 ms after wake, before the
 * bootloader. esp_default_wake_deep_sleep() MUST be called first to keep the
 * system bootable; everything after is best-effort and always releases the bus. */
void RTC_IRAM_ATTR esp_wake_deep_sleep(void)
{
    esp_default_wake_deep_sleep();

    g_touch_wake_capture.runs++;
    g_touch_wake_capture.stage = TWS_STAGE_PINS;

    /* GPIO19/20 double as the S3's USB D-/D+ pads, and the deep-sleep wake reset
     * re-enables the USB pad function, which DISCONNECTS them from the GPIO
     * matrix (the normal gpio driver clears this bit for us at full boot; here we
     * must do it ourselves or every bit-bang goes nowhere). */
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);

    tws_pin_init(TWS_SCL);
    tws_pin_init(TWS_SDA);
    esp_rom_delay_us(TWS_HALF_US);

    uint8_t status = 0;
    g_touch_wake_capture.stage = TWS_STAGE_STATUS_FAIL;
    if (!tws_read_reg(TWS_REG_STATUS, &status, 1)) goto done;
    g_touch_wake_capture.status = status;
    g_touch_wake_capture.stage = TWS_STAGE_NO_POINT;
    if (!(status & 0x80) || (status & 0x0f) == 0) goto done;   /* no fresh point */

    uint8_t p[4] = {0};
    g_touch_wake_capture.stage = TWS_STAGE_POINT_FAIL;
    if (!tws_read_reg(TWS_REG_POINT1, p, 4)) goto done;

    g_touch_wake_capture.rx = (int32_t)(p[0] | (p[1] << 8));
    g_touch_wake_capture.ry = (int32_t)(p[2] | (p[3] << 8));
    g_touch_wake_capture.magic = TOUCH_WAKE_MAGIC;
    g_touch_wake_capture.stage = TWS_STAGE_CAPTURED;

done:
    tws_release(TWS_SDA);
    tws_release(TWS_SCL);
}

#endif /* BOARD_TOUCH_WAKE_STUB */
