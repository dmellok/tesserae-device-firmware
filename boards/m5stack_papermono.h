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
 * keys repaint the panel. Touch, the microSD deck cache, the frontlight, the
 * red status LED and partial refresh were wired after that first pass; see
 * their sections for what each has been seen doing.
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
/* Touch -- FocalTech FT6336G on the system I2C bus                        */
/* ------------------------------------------------------------------ */
/* Address 0x38, INT on GPIO4 (an RTC pad, so it joins the button ext1
 * ANY_LOW wake mask), reset on expander PYG6 (index 5) and power on expander
 * PYG13 (index 12). Driven by touch_ft6336.c (BOARD_TOUCH_FT6336), which
 * takes the place of the GT911 driver behind the same touch_*() API. The
 * controller reports in the 480x800 portrait frame directly (M5GFX: x
 * 0..479, y 0..799, offset_rotation 0), so no swap and no inversion; the
 * selftest confirmed it (top-left tap -> (34,63), bottom-right -> (458,762);
 * chip 0x64, vendor 0x11, fw 0x13). Both expander lines keep their
 * state through the MCU's deep sleep, so the digitiser stays powered and a
 * touch pulls INT low to wake the board; the controller's own monitor mode
 * is what bounds the standing draw. Enabled at runtime by the server
 * (touch_enabled), off by default like every touch panel here. */
#define BOARD_HAS_TOUCH              1
#define BOARD_TOUCH_FT6336           1
#define BOARD_TOUCH_I2C_PORT         0
#define BOARD_TOUCH_I2C_SDA          47
#define BOARD_TOUCH_I2C_SCL          48
#define BOARD_TOUCH_I2C_HZ           400000
#define BOARD_TOUCH_I2C_ADDR         0x38
#define BOARD_TOUCH_INT_PIN          4
#define BOARD_TOUCH_RST_M5IOE1_PIN   5     /* PYG6:  TP_RST, active low */
#define BOARD_TOUCH_EN_M5IOE1_PIN    12    /* PYG13: TP_VDD_EN, active high */
#define BOARD_TOUCH_FRAME_W          480
#define BOARD_TOUCH_FRAME_H          800
#define BOARD_TOUCH_SWAP_XY          0
#define BOARD_TOUCH_INVERT_X         0
#define BOARD_TOUCH_INVERT_Y         0

/* ------------------------------------------------------------------ */
/* microSD -- SDMMC on dedicated pins, rail and detect on the expander     */
/* ------------------------------------------------------------------ */
/* CLK 13 / CMD 12 / D0 11 (D1-D3 on 10/9/8 unused: mounted 1-bit like the
 * Waveshare boards), so the deck cache shares nothing with the panel. Slot
 * power (TF_EN, PYG14 / index 13) and card detect (TF_DET, PYG1 / index 0,
 * active low) are expander pins; sdcard.c drives them through m5ioe1.
 * Verified 2026-09-12: a 16 GB card mounts at 20 MHz, write + digest +
 * read-back pass (m5stack-papermono-sdtest). */
#define TESSERAE_SD_SLOT      1
#define SD_USE_SDMMC          1
#define SD_MMC_PIN_CLK        13
#define SD_MMC_PIN_CMD        12
#define SD_MMC_PIN_D0         11
#define SD_EN_M5IOE1_PIN      13
#define SD_DET_M5IOE1_PIN     0
/* Never cut the slot rail once it is up. With the rail switched off after
 * every cycle the bench unit power-cycled (reset reason POWERON) with a card
 * fitted and ran clean without one (2026-09-13); M5's own firmware raises
 * TF_EN once at boot and leaves it. Costs the card's idle current. */
#define SD_RAIL_KEEP          1

/* ------------------------------------------------------------------ */
/* Frontlight and status LED, both through the M5PM1                       */
/* ------------------------------------------------------------------ */
/* Frontlight: PWM0 on the PMIC's GPIO3 at 5 kHz (M5's PowerDemo). Server
 * config `frontlight_pct` (0-100, default 0); the PMIC keeps the PWM running
 * through the MCU's deep sleep, so a lit panel stays lit until set to 0.
 * Status LED: the red channel of the RGB LED is the PMIC's LED_EN line, so
 * led.c gets its boot indicator through m5pm1_led_set(); green and blue sit
 * on expander PWM channels and stay dark. */
#define BOARD_FRONTLIGHT_M5PM1   1
#define BOARD_LED_M5PM1          1

/* ------------------------------------------------------------------ */
/* Also on the board, unused                                             */
/* ------------------------------------------------------------------ */
/* PDM mic, BMI270, RX8130CE RTC, SX1262 LoRa (SPI on 38/39/40/41, BUSY 21,
 * IRQ 5, Pro only) and ST25R3916 NFC (IRQ 6, Pro only) have no consumer in
 * this firmware. The RX8130CE could seed the clock like the reTerminals'
 * PCF8563 does; different register map, a follow-up. */

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

/* Local overlay render mode (overlay.h): the SSD1677's windowed partial
 * waveform, as on the Sticky -- tap echo, touch-v3 primitives, live value
 * slots and frame patches, with quality repaints falling back to a full
 * 4-gray paint. M5 advises a full refresh after about ten partials on this
 * glass, which the driver's hygiene repaint already covers. With
 * BOARD_HAS_TOUCH above this also enables touch v3. Verified 2026-09-12
 * (m5stack-papermono-overlaytest): tiles invert and a digit slot counts
 * cleanly at ~0.78 s a window against 3.9 s for a full paint, with the
 * rest of the page undisturbed. */
#define BOARD_OVERLAY_PARTIAL 1

/* Selected panel driver: Family E, SSD1677 grayscale over SPI. */
#define PANEL_DRIVER_SSD1677_GRAY 1
