/*
 * Board: paperlesspaper OpenPaper 7 (7.3" Spectra 6 e-paper picture frame)
 *   - MCU:   ESP32-C6-MINI-1-N4 (RISC-V, 4 MB flash, ~512 KB SRAM, NO PSRAM),
 *            CP2102N USB-UART bridge on UART0 (no native USB on the port)
 *   - Panel: 7.3" 800x480 Spectra 6 (GDEP073E01 / EL073TF1 class), single
 *            controller, 4bpp packed = 192000 bytes
 *   - Power: 4 x AAA NiMH behind a BQ25172 charger (NOT a Li-Po: every
 *            voltage threshold below is a 4-cell NiMH figure)
 *
 * Pin map from the vendor firmware (paperlesspaper/paperlesspaper-firmware,
 * src/main.cpp + src/epaper_display.h) and the open PCB schematic
 * (paperlesspaper/paperlesspaper-hardware, paper7/pcb/EPAPER73_V2_sch.jpg).
 * Both agree; the only thing this header changes is the units of the battery
 * divider (see below).
 *
 * The frame has two buttons. The big one is wired to the module's EN pin, so a
 * press is a hardware reset, which the firmware already treats as a "wake and
 * repaint" (cold boot -> fresh frame fetch). The small one is BOOT (GPIO9),
 * only for entering the download mode. Neither is an ext1 button, so there is
 * no BOARD_BTN_* here and no Bluetooth setup gesture; setup is the captive
 * portal.
 *
 * Also on the board, not driven by this firmware yet: a KXTJ3-1057
 * accelerometer (I2C SDA=6 SCL=7, INT=GPIO0, used by the vendor for
 * orientation + shake-to-wake), a FUSB303 USB-C controller (I2C 0x21, exposes
 * VBUS presence), a W25Q128 16 MB SPI flash (CS=21, the vendor's image
 * staging area), a green LED on GPIO14, and a 7-pin expansion header carrying
 * the SPI bus plus GPIO12/13 (the vendor's SD card slot; no card in the frame).
 *
 * CONFIRMED ON HARDWARE 2026-09-09: colour bars, setup splash, portal
 * onboarding and a full frame cycle against a Tesserae server. The panel init
 * sequence is the vendor's own (EPD_S6_INIT_GDEP073E01_V2). Flashing note:
 * this chip revision (v0.2) rejects esptool's flasher stub, so every esptool
 * call needs --no-stub at 115200, and there is no DTR/RTS auto-reset (hold
 * Boot, tap Reset). Battery scaling is still the vendor's figure, unmetered.
 */
#pragma once

/* ------------------------------------------------------------------ */
/* Panel pin map (ESP32-C6 GPIO numbers)                                */
/* ------------------------------------------------------------------ */
#define EPD_PIN_SCLK   15
#define EPD_PIN_MOSI   4
#define EPD_PIN_CS     20   /* single controller chip-select */
#define EPD_PIN_DC     19
#define EPD_PIN_RST    1
#define EPD_PIN_BUSY   18   /* active low: 0 = busy */

/* The C6 has one general-purpose SPI host. */
#define EPD_SPI_HOST   SPI2_HOST
#define EPD_SPI_HZ     (10 * 1000 * 1000)   /* vendor DISPLAY_SPI_SPEED */

/* Panel geometry. Landscape-native 800x480, 4bpp packed = 1 controller. */
#define EPD_WIDTH      800
#define EPD_HEIGHT     480
#define EPD_BUF_BYTES  ((EPD_WIDTH * EPD_HEIGHT) / 2)   /* 4bpp packed = 192000 */

/* 6-colour Spectra palette indices (nibble values). */
#define EPD_COL_BLACK   0x0
#define EPD_COL_WHITE   0x1
#define EPD_COL_YELLOW  0x2
#define EPD_COL_RED     0x3
#define EPD_COL_BLUE    0x5
#define EPD_COL_GREEN   0x6

/* The glass is mounted upside down in the frame (the vendor firmware draws
 * pictures at GxEPD2 rotation 2). Bench 2026-09-09: the selftest bands came
 * out in reverse order until this was set. Same mechanism as the PhotoPainter:
 * the driver reverses the byte stream and swaps nibbles on the way out. */
#define EPD_ROTATE_180 1

/* Use the vendor's GDEP073E01 init block (Good Display's 2024 reference:
 * 6-byte PWR, IPC, VDCS, CCSET, TSSET) rather than the older E1002 block the
 * single driver ships by default. The vendor runs it on both panel batches
 * they source (their firmware tells "DKE" from "OKRA" glass by register 0x9A
 * and uses the same init for both). If the bars come out wrong on a unit,
 * drop this define to fall back to the E1002 sequence. */
#define EPD_S6_INIT_GDEP073E01_V2 1

/* Board model -> default device id "OpenPaper_7_<mac-suffix>". */
#define TESSERAE_DEVICE_MODEL  "OpenPaper_7"

/* Tesserae hardware-catalog kind (esp32_client protocol + esp32_bin renderer:
 * 800x480 4bpp Spectra-6, 192000 bytes; hardware/paperlesspaper/openpaper_7.json). */
#define TESSERAE_DEVICE_KIND   "paperlesspaper_openpaper_7"

/* Cloud-relay self-report (docs/relay/contract.md, POST /v1/pair). */
#define TESSERAE_RELAY_MODEL   "esp32_client"
#define TESSERAE_RELAY_GAMUT   "spectra_6"

/* Battery sense: pack voltage through a 510k/510k divider (R39/R40) into
 * GPIO2 = ADC1 channel 2, behind a P-channel load switch (DMP2035U) whose gate
 * is GPIO3: LOW turns the switch ON. The resistor ratio is 2:1, but the
 * vendor's own calibration reads about 2.8:1 (their raw-count factor 2.28
 * against a 3.3 V full scale): with 510k of source impedance the ADC's
 * sample capacitor under-reads, so we start from the vendor's figure. Check
 * against a meter on the first bench pass and trim BOARD_BATTERY_DIVIDER_X100. */
#define BOARD_BATTERY_ADC_CHANNEL     ADC_CHANNEL_2
#define BOARD_BATTERY_DIVIDER_X100    283
#define BOARD_VBAT_SWITCH_PIN         3
#define BOARD_VBAT_SWITCH_ACTIVE_LOW  1

/* 4 x AAA NiMH: 5.6 V fresh off the charger, 4.8 V nominal, 4.2 V empty
 * (1.05 V/cell, the vendor's BAT_OFF_VALUE). Drives battery_pct() and every
 * mV threshold that would otherwise assume a Li-Po cell. */
#define BOARD_BATTERY_NIMH_CELLS      4
#define BATTERY_GOODBYE_MV            4200
#define BATTERY_RESUME_MV             4500
#define TESSERAE_OTA_MIN_BATTERY_MV   4600

/* The USB-C port is a CP2102N UART bridge; the C6's own USB-Serial-JTAG pads
 * (GPIO12/13) go to the expansion header instead. Never consult that
 * peripheral for "is a laptop attached" (main.c dev-loop detection). */
#define BOARD_USB_SERIAL_JTAG_ABSENT  1

/* MCU tier: ESP32-C6, single-core RISC-V, no PSRAM. The 192000-byte frame
 * comes from internal RAM (TESSERAE_FB_CAPS); image_fetcher pre-sizes its
 * download buffer to exactly one frame on this tier so it is never doubled. */
#define MCU_TIER_C6_NO_PSRAM 1

/* Selected panel driver: Family B, single-controller Spectra-6. */
#define PANEL_DRIVER_SPECTRA6_SPI_SINGLE 1
