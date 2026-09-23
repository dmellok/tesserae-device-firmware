/*
 * sdcard.h: runtime-probed microSD mount for the deck frame cache.
 *
 * Boards with a slot define TESSERAE_SD_SLOT plus a pin map in boards/*.h:
 *
 *   SDMMC boards (dedicated pins, both Waveshares):
 *     #define TESSERAE_SD_SLOT 1
 *     #define SD_USE_SDMMC     1
 *     #define SD_MMC_PIN_CLK   <gpio>
 *     #define SD_MMC_PIN_CMD   <gpio>
 *     #define SD_MMC_PIN_D0    <gpio>     // mounted 1-bit for robustness
 *
 *   SPI boards (reTerminal E10xx; the card SHARES the panel SPI bus --
 *   EPD_SPI_HOST / EPD_PIN_SCLK / EPD_PIN_MOSI -- with its own CS):
 *     #define TESSERAE_SD_SLOT   1
 *     #define SD_SPI_SHARED_BUS  1
 *     #define SD_PIN_MISO  <gpio>         // SD-only; the panel is write-only
 *     #define SD_PIN_CS    <gpio>
 *     #define SD_PIN_DET   <gpio>         // optional: card-detect, active low
 *     #define SD_PIN_EN    <gpio>         // optional: slot power, active high
 *
 *   Either flavour may put the rail and card-detect on an M5IOE1 expander
 *   instead of GPIOs (M5Stack PaperMono):
 *     #define SD_EN_M5IOE1_PIN   <index>   // slot power, active high
 *     #define SD_DET_M5IOE1_PIN  <index>   // card-detect, active low
 *     #define SD_RAIL_KEEP       1         // optional: raise the rail once, never cut it
 *
 * Everything is RUNTIME gated: no card / no slot / mount failure all degrade
 * to "capability absent" and the wake loop behaves exactly as before. Boards
 * without TESSERAE_SD_SLOT compile this to no-op stubs.
 *
 * Bus discipline on shared-SPI boards: the ESP-IDF SPI driver serialises
 * transactions between the sdspi device and the panel's device, and the wake
 * loop is single-threaded -- SD I/O happens strictly before or after a panel
 * refresh, never during. sdcard_mount() initialises the shared bus with the
 * panel's own transfer cap so whichever side comes second tolerates
 * ESP_ERR_INVALID_STATE and inherits a bus that fits full frames.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"   /* -> boards/board.h: TESSERAE_SD_SLOT + pins */

#define SDCARD_MOUNT_POINT "/sdcard"

#if defined(TESSERAE_SD_SLOT)

/* Park the SD lines FIRST THING at boot, before anything touches the shared
 * SPI bus: CS driven high (deselected) and the slot rail off. With a card
 * inserted and CS left floating, the card sits half-selected on the panel's
 * bus -- bench E1001 (2026-07-23): panel refreshes ran 2.5x slow and moved no
 * ink until the card was pulled. Harmless no-op on dedicated-pin boards. */
void sdcard_quiesce(void);

/* Probe + mount the card (idempotent). False when no card is present, the
 * mount fails, or FATFS is unreadable -- callers just skip the cache. */
bool sdcard_mount(void);

/* True while a card is mounted. */
bool sdcard_mounted(void);

/* Unmount + power down the slot (where the board gates it). Safe when not
 * mounted. Use sdcard_prepare_sleep() on the deep-sleep path. */
void sdcard_unmount(void);

/* sdcard_unmount() plus a LATCH on the slot-power pin, so the rail stays
 * commanded off for the whole deep sleep instead of going hi-Z with the rest
 * of the pads. A load-switch enable left floating is undefined by the part's
 * own datasheet, and this board family has an unexplained sleep draw
 * (server #327), so the assumption that "hi-Z reads as off" is one worth not
 * making. gpio_hold_dis() on every path that raises the rail again keeps the
 * latch from swallowing the next mount. No-op where the rail is an expander
 * pin (PaperMono) or the board asks to keep it up (SD_RAIL_KEEP). */
void sdcard_prepare_sleep(void);

/* Free bytes on the mounted filesystem, 0 when unmounted. */
uint64_t sdcard_free_bytes(void);

/* Raw card handle for diagnostics (sdmmc_card_t*), NULL when unmounted. */
void *sdcard_handle(void);

#else /* no slot wired: compile the feature out cold */

static inline void     sdcard_quiesce(void)    { }
static inline bool     sdcard_mount(void)      { return false; }
static inline bool     sdcard_mounted(void)    { return false; }
static inline void     sdcard_unmount(void)    { }
static inline void     sdcard_prepare_sleep(void) { }
static inline uint64_t sdcard_free_bytes(void) { return 0; }
static inline void    *sdcard_handle(void)     { return 0; }

#endif /* TESSERAE_SD_SLOT */
