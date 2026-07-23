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

/* Probe + mount the card (idempotent). False when no card is present, the
 * mount fails, or FATFS is unreadable -- callers just skip the cache. */
bool sdcard_mount(void);

/* True while a card is mounted. */
bool sdcard_mounted(void);

/* Unmount + power down the slot (where the board gates it). Call before
 * every deep sleep. Safe when not mounted. */
void sdcard_unmount(void);

/* Free bytes on the mounted filesystem, 0 when unmounted. */
uint64_t sdcard_free_bytes(void);

#else /* no slot wired: compile the feature out cold */

static inline bool     sdcard_mount(void)      { return false; }
static inline bool     sdcard_mounted(void)    { return false; }
static inline void     sdcard_unmount(void)    { }
static inline uint64_t sdcard_free_bytes(void) { return 0; }

#endif /* TESSERAE_SD_SLOT */
