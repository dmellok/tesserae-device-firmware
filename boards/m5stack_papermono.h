/*
 * Board: M5Stack PaperMono (SKU C153) and PaperMono Lite (C153-Lite)
 *   - MCU:   ESP32-S3R8 (8 MB octal PSRAM, 16 MB flash, native USB)
 *   - Panel: 3.97" 480x800, 4-level grayscale, SSD1677 over SPI
 *   - Touch: FocalTech FT6336G on the system I2C bus (NOT wired up here)
 *
 * Family E, the reTerminal Sticky's driver (ssd1677_gray) on the same glass
 * and controller family: 800x480 SSD1677 scan, portrait mount, 96000-byte
 * 2bpp frame. The Lite drops LoRa and NFC and is otherwise the same board, so
 * both run this one target under one catalog kind.
 *
 * WHAT IS DIFFERENT FROM THE STICKY. Two of the panel's control lines are not
 * ESP32 GPIOs at all: the SSD1677's reset and its 3.3 V enable hang off an
 * M5IOE1 I2C IO expander, and the touch controller's reset and power, the
 * microSD rail and two of the RGB LED channels sit behind the same chip. The
 * ssd1677_gray driver therefore takes EPD_RST_M5IOE1_PIN / EPD_EN_M5IOE1_PIN
 * (expander pin indices) in place of EPD_PIN_RST / EPD_PIN_EN, routed through
 * src/m5ioe1.c. Power is owned by an M5PM1 PMIC: the side power button is the
 * PMIC's, not a GPIO, and the PMIC keeps the rails up on its own (M5Unified
 * never writes a hold register), so there is no power latch here. The PMIC
 * also measures the cell, which is how battery telemetry works on this board
 * (src/m5pm1.c).
 *
 * Pin map, expander channel assignments and addresses from M5Stack's docs
 * (docs.m5stack.com/en/core/PaperMono, schematic V0.6.2 2026-05-22),
 * cross-checked against M5GFX src/M5GFX.cpp (board_M5PaperMono, Panel
 * SSD1677 cfg), M5Unified src/M5Unified.cpp (buttons, mic, I2C) and M5's
 * own ESP-IDF projects (M5PaperMono-UserDemo hal_board.cpp and
 * M5PaperMono-PowerDemo m5pm_power.cpp, which pulse the panel reset through
 * the expander exactly as done here). All sources agree pin for pin.
 *
 * VERIFIED ON HARDWARE 2026-09-12 (a PaperMono Pro on the bench): the
 * expander answers and powers/resets the panel, the four-grey selftest ramp
 * paints with the wedge top-left, the captive portal, discover, register and
 * a real 480x800 dashboard frame over the v1 REST API (upright, unmirrored,
 * correct greys), and battery telemetry from the PMIC. Two things the first
 * pass got wrong, both now board knobs on the driver: the grey OTP waveform
 * is selected with temperature value 0x5A, not the Sticky's 0x67 (which paints
 * static here), and the mid-grey planes are the Sticky's swapped. Both side
 * keys repaint the panel. Not yet exercised: the buzzer, deep sleep on
 * battery, BLE setup.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (ESP32-S3 GPIO numbers, from M5's PaperMono pinmap)   */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   15
#define EPD_PIN_MOSI   14
#define EPD_PIN_CS     16
#define EPD_PIN_DC     17
#define EPD_PIN_BUSY   18   /* active HIGH: 1 = busy (SSD1677) */

/* Reset and 3.3 V enable are M5IOE1 expander outputs, not GPIOs. Indices are
 * the chip's 0-based pin numbers: M5's PYG3 (EPD_3V3_EN) is index 2, PYG5
 * (EPD RST) is index 4. EPD_PIN_RST / EPD_PIN_EN stay undefined on purpose;
 * the driver switches to the expander when these are present. */
#define EPD_EN_M5IOE1_PIN   2    /* PYG3: SSD1677 3.3 V rail, active high */
#define EPD_RST_M5IOE1_PIN  4    /* PYG5: SSD1677 reset, active low */

#define EPD_SPI_HOST   SPI2_HOST
/* 4 MHz, the Sticky's proven rate on this controller. M5GFX writes at 40 MHz
 * on this very board, so there is headroom to raise once the glass is seen
 * painting; a 96000-byte frame is ~0.2 s at 4 MHz per plane, so the paint
 * time is the waveform, not the bus. */
#define EPD_SPI_HZ     (4 * 1000 * 1000)

/* Panel geometry, in FRAME coordinates: what the server renders and what the
 * user sees. 480x800 portrait, 2bpp packed = 4 px/byte = 96000 bytes.
 *
 * The CONTROLLER scans 800x480 landscape (M5GFX: panel_width 800,
 * panel_height 480, offset_rotation 3) and the glass is mounted portrait, so
 * the driver transposes on the way out, as it does on the Sticky. Whether
 * this glass wants the Sticky's exact transpose or a mirrored one is the
 * open question for the first unit: EPD_MIRROR_Y is the knob. */
#define EPD_WIDTH      480
#define EPD_HEIGHT     800
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 4)   /* 2bpp packed = 96000 */

/* Controller scan geometry, always landscape regardless of the mount. */
#define EPD_PANEL_SCAN_W  800
#define EPD_PANEL_SCAN_H  480

/* 4-gray palette (linear), matching the Sticky and the E1001 gray board. */
#define EPD_COL_BLACK      0x0
#define EPD_COL_DARKGRAY   0x1
#define EPD_COL_LIGHTGRAY  0x2
#define EPD_COL_WHITE      0x3

/* Grayscale OTP waveform selection, from M5Stack's own OTP demo for this
 * glass (M5PaperMono-OTP-Demo, init_gray_mode): temperature value 0x5A
 * selects the 4-gray LUT (the Sticky's 0x67 painted static here on the
 * bench, 2026-09-12), and the mid greys sit in the opposite planes to the
 * Sticky's table (light gray = plane1 set, dark gray = plane2 set). */
#define EPD_SSD1677_GRAY_TEMP       0x5A
#define EPD_SSD1677_GRAY_MID_SWAP   1

/* ------------------------------------------------------------------ */
/* System I2C bus: PMIC, IO expander, RTC, IMU, touch, NFC              */
/* ------------------------------------------------------------------ */
/* One bus (SDA 47 / SCL 48) carries everything: M5PM1 PMIC 0x6E, M5IOE1
 * expander 0x4F, RX8130CE RTC 0x32, BMI270 IMU 0x68, FT6336G touch 0x38,
 * ST25R3916 NFC 0x50 (Pro only), and the IP2315 charger at 0x75 behind an
 * expander-gated pull-up. Every consumer goes through i2c_bus_get(). 100 kHz
 * is what M5's own demo runs the PMIC and expander at. */
#define BOARD_M5IOE1_I2C_PORT   0
#define BOARD_M5IOE1_I2C_SDA    47
#define BOARD_M5IOE1_I2C_SCL    48
#define BOARD_M5IOE1_I2C_HZ     100000
#define BOARD_M5IOE1_I2C_ADDR   0x4F

/* Battery: the M5PM1 measures VBAT and publishes millivolts over I2C. No
 * divider reaches an ESP32 ADC pin on this board, so the ADC backend can never
 * work here. 1150 mAh cell, charged by an IP2315 the PMIC supervises. */
#define BOARD_BATTERY_M5PM1     1
#define BOARD_M5PM1_I2C_PORT    0
#define BOARD_M5PM1_I2C_SDA     47
#define BOARD_M5PM1_I2C_SCL     48
#define BOARD_M5PM1_I2C_HZ      100000
#define BOARD_M5PM1_I2C_ADDR    0x6E

/* ------------------------------------------------------------------ */
/* Buttons: two user keys on the side, plus the PMIC's power button      */
/* ------------------------------------------------------------------ */
/* USER_KEY1 (A) on GPIO2 and USER_KEY2 (B) on GPIO3, active low; M5's
 * deep-sleep test arms exactly these two as an ext1 mask with the internal
 * pull-ups on, which is what buttons.h does. Both are RTC pads, so both wake.
 *
 * Mapped as previous / next: on a two-key handheld cycling dashboards is the
 * useful pair, and any button wake already forces a fresh paint. KEY1 is
 * declared as the REFRESH pin because BLE setup and the settings-mode hold
 * key off that slot (ble_setup.c refuses to build without one), and reported
 * as "left" so the server still binds it to rotate_prev -- the Xteink X4
 * plays the same trick. The power button belongs to the M5PM1 (single press
 * reset, double press off, per the PMIC's defaults) and is not a GPIO. */
#define BOARD_BTN_REFRESH_PIN        2       /* KEY1 (A): rotate prev, setup hold */
#define BOARD_BTN_REFRESH_WIRE_NAME  "left"
#define BOARD_BTN_RIGHT_PIN          3       /* KEY2 (B): rotate next */

/* Passive buzzer on GPIO42 (M5: BB_PWM), LEDC-driven like the Sticky's. */
#define BOARD_BUZZER_PIN       42

/* ------------------------------------------------------------------ */
/* Touch -- FT6336G, present, NOT enabled                                */
/* ------------------------------------------------------------------ */
/* A FocalTech FT6336G at 0x38 on the system bus, INT on GPIO4 (an RTC pad,
 * so it could join the ext1 wake mask), reset on expander PYG6 (index 5) and
 * power on expander PYG13 (index 12). It reports in the 480x800 portrait
 * frame directly (M5GFX: x 0..479, y 0..799, offset_rotation 0; M5 quotes an
 * active area of 5..475 x 5..795). Not wired: this firmware's touch path is
 * the Goodix GT911 driver, and the FT6336 has a different register map
 * (touch count at 0x02, first point at 0x03..0x06). A touch_ft6336.c behind
 * the same touch_*() API, plus expander-driven reset/power in its init, is
 * the follow-up; the panel comes first. BOARD_HAS_TOUCH stays undefined so
 * main.c's touch paths compile out. */

/* ------------------------------------------------------------------ */
/* microSD -- present, NOT enabled                                       */
/* ------------------------------------------------------------------ */
/* Dedicated SDMMC pins (CLK 13, CMD 12, D0 11, D1 10, D2 9, D3 8), so the
 * deck cache would take the SD_USE_SDMMC path with no bus sharing at all.
 * The slot's power (TF_EN, expander PYG14 / index 13) and card detect
 * (TF_DET, PYG1 / index 0) are expander pins, and sdcard.c drives SD_PIN_EN
 * as a GPIO, so enabling it needs an expander-aware rail hook there first.
 * Left off for bring-up; the cache is an optimisation. */

/* ------------------------------------------------------------------ */
/* Also on the board, unused                                             */
/* ------------------------------------------------------------------ */
/* Frontlight: PWM on the PMIC's GPIO3 (M5PM1 PWM0), off unless driven, so
 * the panel behaves like every other unlit e-paper here. RGB LED: red via
 * the PMIC's LED_EN, green/blue via expander PWM channels; led.c wants a GPIO,
 * so no status LED. PDM mic, BMI270, RX8130CE RTC, SX1262 LoRa (SPI on
 * 38/39/40/41, BUSY 21, IRQ 5, Pro only) and ST25R3916 NFC (IRQ 6, Pro only)
 * have no consumer in this firmware. The RX8130CE could seed the clock like
 * the reTerminals' PCF8563 does; different register map, a follow-up. */

/* Board model -> default device id "PaperMono_<mac-suffix>". Same model for
 * the Pro and Lite variants. */
#define TESSERAE_DEVICE_MODEL  "PaperMono"
/* Next free code; 1-9 and 11 are taken. Identifies the hardware over BLE
 * setup. */
#define TESSERAE_BLE_HARDWARE_CODE  12

/* Tesserae hardware-catalog kind.
 *
 * SERVER DEPENDENCY: catalog entry hardware/m5stack/papermono.json -- the
 * Sticky's wire contract (protocol esp32_bw_client, gamut gray_4, renderer
 * esp32_gray2_bin, 480x800 native so the server packs 120-byte portrait rows
 * for the firmware to transpose). A kind is also the OTA lineage, so it is
 * fixed from first release and the Lite shares it. */
#define TESSERAE_DEVICE_KIND   "m5stack_papermono"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). Identical
 * to the Sticky's and the Xteink X4 gray build's on the wire, which is why the
 * catalog entry is auto_select:false: the server pins this kind only from the
 * TESSERAE_DEVICE_KIND declared at register/discover, never by inference. */
#define TESSERAE_RELAY_MODEL   "esp32_bw_client"
#define TESSERAE_RELAY_GAMUT   "gray_4"

/* MCU tier: ESP32-S3R8, 8 MB octal PSRAM (M5 spec sheet; M5GFX notes the
 * board needs OPI PSRAM enabled, which sdkconfig.defaults does). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* No BOARD_OVERLAY_PARTIAL yet. The SSD1677's windowed partial waveform is
 * what gives the Sticky its live slots and frame patches, and this is the
 * same controller family, so it should carry over -- but it is enabled only
 * after the full-paint path is seen working on a real unit, and M5 itself
 * advises a full refresh after about ten partials on this glass. */

/* Selected panel driver: Family E, SSD1677 grayscale over SPI. */
#define PANEL_DRIVER_SSD1677_GRAY 1
