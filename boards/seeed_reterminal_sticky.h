/*
 * Board: Seeed reTerminal Sticky
 *   - MCU:   ESP32-S3, 8 MB PSRAM, 32 MB QSPI flash
 *   - Panel: 3.97" 800x480, 4-level grayscale, SSD1677 over SPI
 *   - Touch: Goodix GT911 capacitive, on its own I2C bus (see below)
 *
 * Family E. Uses the ssd1677_gray driver.
 *
 * HOW THE CONTROLLER WAS IDENTIFIED. Seeed's Sticky documentation gives the
 * GPIO map but never names the display controller, and there is no Sticky entry
 * in Seeed_GFX. The pin map settles it: the documented panel pins here are
 * identical, pin for pin, to Seeed_GFX's Setup524_Seeed_reTerminal_E1005.h,
 * which declares SSD1677_DRIVER at 800x480. The Sticky and the (unannounced)
 * E1005 are the same board design, so the controller is SSD1677 -- and the same
 * driver should also cover the XIAO ePaper 3.97", whose Seeed_GFX setup names
 * the same part.
 *
 * UNVERIFIED ON HARDWARE. The pin map is from Seeed's docs and the sequences
 * are ported from Seeed_GFX, but nothing here has been run on a panel. Flash
 * the -selftest env first: four grey bands, black at the top through white at
 * the bottom. If the ends are swapped, the plane encoding in ssd1677_gray.c is
 * inverted for this glass; if nothing refreshes at all, suspect BUSY polarity
 * (this controller is busy-HIGH, the opposite of the UC8179 panels).
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (from Seeed's Sticky hardware overview).              */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   13
#define EPD_PIN_MOSI   14
#define EPD_PIN_CS     15
#define EPD_PIN_DC     16
#define EPD_PIN_RST    17
#define EPD_PIN_BUSY   18   /* active HIGH: 1 = busy (opposite of UC8179) */
#define EPD_PIN_EN     47   /* panel power enable */

#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (4 * 1000 * 1000)

/* Panel geometry, in FRAME coordinates: what the server renders and what the
 * user sees. 480x800 portrait, 2bpp packed = 4 px/byte.
 *
 * The CONTROLLER scans 800x480 landscape. The glass is mounted rotated, so the
 * driver transposes on the way out (see ssd1677_gray.c) rather than the server
 * rendering sideways content. Confirmed on hardware: a selftest drawn as
 * horizontal bands in controller space came out as vertical columns reading
 * left to right, which is a 90-degree mount.
 *
 * The buffer is the same 96000 bytes either way, so this is purely a question
 * of which axis is which -- and it matches the 480x800 the server already has
 * recorded for this hardware. */
#define EPD_WIDTH      480
#define EPD_HEIGHT     800
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 96000 */

/* Controller scan geometry, always landscape regardless of how it is mounted.
 * Used for the RAM window and the gate/source setup, never for the frame. */
#define EPD_PANEL_SCAN_W  800
#define EPD_PANEL_SCAN_H  480

/* 4-gray palette (linear), matching the E1001 gray board. */
#define EPD_COL_BLACK      0x0
#define EPD_COL_DARKGRAY   0x1
#define EPD_COL_LIGHTGRAY  0x2
#define EPD_COL_WHITE      0x3

/* Board model -> default device id "reTerminal_Sticky_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "reTerminal_Sticky"
/* Next free code; 1-7 are taken. Identifies the hardware over BLE setup. */
#define TESSERAE_BLE_HARDWARE_CODE  8

/* Tesserae hardware-catalog kind.
 *
 * SERVER DEPENDENCY: needs a catalog entry for seeed_reterminal_sticky. It can
 * be a copy of the E1001 gray entry with this id and 800x480 dims -- same
 * protocol (esp32_bw_client), same panel.gamut (gray_4), same renderer
 * (esp32_gray2_bin), because the frame bytes are the same shape. Without it the
 * panel pairs but never receives a correctly packed frame.
 *
 * NOTE: an existing Sticky may already be registered under a different kind by
 * whatever firmware it shipped with. Moving it to this one is a re-key, not an
 * upgrade. */
#define TESSERAE_DEVICE_KIND   "seeed_reterminal_sticky"

#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "gray_4"

/* Battery: a TI BQ27220 fuel gauge on I2C, NOT a divider to an ADC pin -- there
 * is no such divider on this board, so the ADC backend can never work here.
 *
 * Address and bus are from CrossPoint's Sticky board profile, which puts the
 * gauge at 0x55 on the sensor bus at 400 kHz. That is the same physical bus as
 * the SHT40 below, so both go through i2c_bus_get(); the differing clock rates
 * are fine, since speed is a per-device property in the IDF 5 I2C master API.
 *
 * The BQ25616 charger drives a CHARGE_STATE line on GPIO40. Nothing reports a
 * charging flag today, so it is left unwired rather than guessed at: the gauge
 * can answer the same question from the sign of AverageCurrent() if we ever
 * want it, without a polarity assumption to get wrong. */
#define BOARD_BATTERY_GAUGE_I2C    1
#define BOARD_BATTERY_GAUGE_PORT   0
#define BOARD_BATTERY_GAUGE_SDA    1
#define BOARD_BATTERY_GAUGE_SCL    0
#define BOARD_BATTERY_GAUGE_HZ     400000
#define BOARD_BATTERY_GAUGE_ADDR   0x55

/* Onboard SHT40 on the sensor I2C bus, shared with the IMU, RTC and gauge. */
#define BOARD_HAS_SHT4X            1
#define BOARD_SHT4X_I2C_PORT       0
#define BOARD_SHT4X_I2C_SDA        1
#define BOARD_SHT4X_I2C_SCL        0
#define BOARD_SHT4X_I2C_HZ         100000
#define BOARD_SHT4X_I2C_ADDR       0x44

/* Onboard Goodix GT911 capacitive touch controller.
 *
 * Wiring is from Seeed's Sticky hardware overview and matches CrossPoint's
 * board profile: the GT911 has its OWN I2C bus (SDA GPIO3 / SCL GPIO2), not
 * the sensor bus the gauge and SHT40 share, so it takes the second I2C
 * controller (port 1). TP_INT on GPIO21 is RTC-capable, so it folds into the
 * buttons' ext1 ANY_LOW wake mask exactly as on the E1003. TP_RST is GPIO41.
 *
 * The digitiser is POWER-GATED behind TOUCH_EN on GPIO42 (active high, a load
 * switch). touch_init() drives it high before the reset/probe, and
 * touch_prepare_sleep() latches it high through deep sleep so the controller
 * stays alive to raise INT. GPIO41/42 are digital pads (not RTC), so those
 * holds go through gpio_deep_sleep_hold_en(); if a touch never wakes the
 * board, BOARD_TOUCH_HOLD_RST is the first knob to flip (the E1003 wakes only
 * with its TP_RST unheld, but that board has an external pull-up on the line).
 *
 * Address 0x5d is selected explicitly during reset as on the E1003, and the
 * point-1 block at 0x8150 (coords at byte 0, no track id) matches what
 * CrossPoint found on this same GT911.
 *
 * ORIENTATION VERIFIED ON HARDWARE (selftest corner taps). The digitiser
 * reports portrait coordinates, raw X across the 480 axis and raw Y along the
 * 800 axis, and reads 0..479 x 0..799 against the frame with only Y reversed:
 * a top-left tap logged raw (16,765), top-right (475,787), bottom-left
 * (30,4). So no swap, X straight through, Y inverted. (The derivation from
 * CrossPoint's swap-and-flip transform had also predicted an X inversion; the
 * glass says otherwise, and the glass wins.) Enabled at runtime by the server
 * (touch_enabled config); disabled by default. */
#define BOARD_HAS_TOUCH            1
#define BOARD_TOUCH_I2C_PORT       1
#define BOARD_TOUCH_I2C_SDA        3
#define BOARD_TOUCH_I2C_SCL        2
#define BOARD_TOUCH_I2C_HZ         400000
#define BOARD_TOUCH_INT_PIN        21
#define BOARD_TOUCH_RST_PIN        41
#define BOARD_TOUCH_EN_PIN         42
#define BOARD_TOUCH_HOLD_RST       1
#define BOARD_TOUCH_I2C_ADDR       0x5d
#define BOARD_TOUCH_SWAP_XY        0
#define BOARD_TOUCH_INVERT_X       0
#define BOARD_TOUCH_INVERT_Y       1
/* Deep-sleep wake stub: bit-bang the GT911 from RTC memory ~1 ms after wake so
 * a quick tap is captured before the boot lets the finger lift. SDA/SCL 3/2
 * are in the low GPIO bank the stub needs. Undefine to disable. */
#define BOARD_TOUCH_WAKE_STUB      1

/* Front buttons. The AI/power key doubles as the wake button. */
/* Passive buzzer, driven by LEDC PWM: frequency sets the pitch, duty sets the
 * loudness (server #258). NOT the same pin as the reTerminal E series, which
 * puts it on 45 -- this is per Seeed's own Sticky hardware overview, whose
 * button pins (4/5/6) match the three defined just below. */
#define BOARD_BUZZER_PIN           48

#define BOARD_BTN_REFRESH_PIN  4    /* AI / power */
#define BOARD_BTN_LEFT_PIN     5    /* up   */
#define BOARD_BTN_RIGHT_PIN    6    /* down */

/* microSD shares the panel SPI bus (SCLK 13 / MOSI 14) with its own CS; MISO is
 * SD-only, the panel being write-only. */
#define TESSERAE_SD_SLOT   1
#define SD_SPI_SHARED_BUS  1
#define SD_PIN_MISO   12
#define SD_PIN_CS     8
/* The slot is POWER-GATED. Without driving this high the card is simply
 * unpowered and every mount ends in ESP_ERR_TIMEOUT, which reads like a missing
 * or faulty card rather than a missing pin. Not in Seeed's published GPIO table;
 * from CrossPoint's Sticky board profile, which lists SD_PWR_EN alongside the
 * bus pins. */
#define SD_PIN_EN     10

/* MCU tier: ESP32-S3 + 8 MB PSRAM. */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* Local overlay render mode (overlay.h): the SSD1677's windowed partial
 * waveform (two-tone, differential against a driver-kept shadow plane; see
 * ssd1677_gray.c) gives this board tap echo, touch-v3 primitives, live value
 * slots and frame patches. Hygiene repaints and quality requests fall back
 * to a full 4-gray paint, since the glass has no grayscale partial. With
 * BOARD_HAS_TOUCH above this also enables touch v3. */
#define BOARD_OVERLAY_PARTIAL 1

/* Selected panel driver: Family E, SSD1677 grayscale over SPI. */
#define PANEL_DRIVER_SSD1677_GRAY 1
