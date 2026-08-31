/*
 * Board: Seeed Studio XIAO ePaper Display Board EE03
 *   - MCU:   XIAO ESP32-S3 Plus (ESP32-S3 + PSRAM), native USB (no CH340)
 *   - Panel: 10.3" ED103TC2 grayscale, 1872x1404, via an onboard IT8951E/DX
 *
 * The panel + controller pair is the SAME ED103TC2 + IT8951 as the reTerminal
 * E1003, so this board REUSES the it8951_gray driver unchanged (Family D,
 * 4bpp GC16). Only the pin map differs. Pins cross-checked against two
 * sources: Seeed_GFX (User_Setups/EPaper_Board_Pins_Setups.h,
 * USE_XIAO_EPAPER_DISPLAY_BOARD_EE03: SCLK=D8, MISO=D9, MOSI=D10, CS=44,
 * DC=-1, BUSY=4, RST=38, ENABLE=43) and the EE03 V1.0 schematic
 * (XIAO_ePaper_Display_Board_EE03_V1.0_SCH_251217.pdf): EDP_BUSY=GPIO4,
 * EDP_RES=GPIO38, PWR_EN=GPIO43, SPI0 on GPIO7/8/9 with CS=GPIO44.
 *
 * NOTE: CS (44) and PWR_EN (43) are the ESP32-S3 UART0 RX/TX pins, so this
 * env's sdkconfig (sdkconfig.usbjtag.defaults, wired via platformio.ini)
 * moves the console to USB-Serial-JTAG so UART0 doesn't fight the panel.
 *
 * Frame is the same 1314144-byte 1872x1404 4bpp grayscale layout as the
 * E1003, so the server reuses that renderer under TESSERAE_DEVICE_KIND below.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* IT8951 SPI pin map (XIAO ESP32-S3 Plus GPIO numbers)                */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK    7    /* XIAO D8  */
#define EPD_PIN_MOSI    9    /* XIAO D10 */
#define EPD_PIN_MISO    8    /* XIAO D9; IT8951 is bidirectional (device-info reads) */
#define EPD_PIN_CS      44   /* XIAO D7  */
#define EPD_PIN_RST     38   /* XIAO D11 */
#define EPD_PIN_BUSY    4    /* HRDY: HIGH = ready (opposite of the UC81xx panels) */

/* Single power-enable line. Per the EE03 schematic, one PWR_EN net (GPIO43)
 * gates the T-CON rails (VCC_ITE_3V3 + VD_1V8 via TPS22916/ETA3410) AND the
 * panel drive rail (VSYS -> EDP_Drive -> TPS651851 EPD PMIC), so the E1003's
 * separate EN / VCC_EN pins collapse onto the same GPIO here. The driver
 * toggles both macros; on this board that is the same pin twice, harmless. */
#define EPD_PIN_EN      43
#define EPD_PIN_VCC_EN  43

#define EPD_SPI_HOST    SPI2_HOST
#define EPD_SPI_HZ      (4 * 1000 * 1000)   /* FastEPD run freq; reads need a modest clock */

/* Panel geometry. 1872x1404, 4bpp packed grayscale = 2 px/byte. */
#define EPD_WIDTH       1872
#define EPD_HEIGHT      1404
#define EPD_BUF_BYTES   ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 1314144 */

/* Grayscale levels (nibble values): 0x0 = black ... 0xF = white. */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0xF

/* IT8951 VCOM, in mV (magnitude of the negative VCOM). Same safe default as
 * the E1003 (identical ED103TC2 glass); tune if contrast is off against the
 * value printed on this unit's panel FPC. */
#define EPD_VCOM_MV     1500

/* Battery sense (EE03 schematic, "BAT ADC DETE" block): GPIO1 = ADC1 channel
 * 0, 10K/10K divider (2:1), gated by a TPS22916 load switch on ADC_EN =
 * GPIO6 (active-high) -- the same circuit and pins as the EE02/EE04 boards. */
#define BOARD_BATTERY_ADC_CHANNEL  ADC_CHANNEL_0
#define BOARD_BATTERY_DIVIDER      2
#define BOARD_VBAT_SWITCH_PIN      6

/* Onboard SHT40 temperature/humidity sensor (EE03 schematic: SDA=IO42,
 * SCL=IO41 through 0R links, address 0x44). Unlike the reTerminals' GPIO19/20
 * bus -- the EE03 routes I2C on the JTAG-capable pins instead. */
#define BOARD_HAS_SHT4X            1
#define BOARD_SHT4X_I2C_PORT       0
#define BOARD_SHT4X_I2C_SDA        42
#define BOARD_SHT4X_I2C_SCL        41
#define BOARD_SHT4X_I2C_HZ         100000
#define BOARD_SHT4X_I2C_ADDR       0x44

/* Front buttons: BUTTON1/2/3 on GPIO2/3/5 per the EE03 schematic (10K
 * pull-ups, active-low, RTC-capable -> ext1 deep-sleep wake sources). Same
 * layout as the EE02/EE04 boards; the keys are only numbered, so the action
 * assignment follows those: Button1=refresh, Button2=left, Button3=right.
 * TODO(verify on hardware): confirm the physical key order matches. */
#define BOARD_BTN_REFRESH_PIN  2
#define BOARD_BTN_LEFT_PIN     3
#define BOARD_BTN_RIGHT_PIN    5

/* Board model -> default device id "Seeed_EE03_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "Seeed_EE03"
#define TESSERAE_BLE_HARDWARE_CODE  11

/* Tesserae hardware-catalog kind. Same 1872x1404 4bpp grayscale frame
 * (1314144 bytes) as the E1003, so the server maps this to the existing
 * esp32_gray_bin renderer. Distinct kind: a kind names an OTA lineage. */
#define TESSERAE_DEVICE_KIND   "seeed_ee03"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair).
 * Values come from this board's entry in the Tesserae hardware
 * catalog (hardware/seeed/ee03.json): "protocol" is the device KIND
 * that selects the renderer/.bin packer, and "panel.gamut" is the
 * palette the server quantizes to. Both are hardware facts, so the
 * panel reports them at pairing and the operator no longer has to
 * pre-enter them.
 *
 * NOTE the kind is NOT the catalog id above -- that is a hardware id, not a
 * device kind. Getting this wrong silently mis-packs every frame. */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "gray_16"

/* MCU tier: ESP32-S3 + octal PSRAM (XIAO ESP32-S3 Plus, 8 MB). */
#define MCU_TIER_S3_OCTAL_PSRAM 1

/* No touch overlay on this board (bare ED103TC2 glass, no GT911), so
 * BOARD_OVERLAY_PARTIAL stays off for bring-up even though the IT8951 has
 * usable partial refresh; enable later if live value slots earn their bench
 * verification. No microSD slot, no buzzer per the V1.0 schematic. */

/* Selected panel driver: Family D, IT8951 grayscale over SPI (shared w/ E1003). */
#define PANEL_DRIVER_IT8951_GRAY 1
