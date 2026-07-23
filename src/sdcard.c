/* sdcard.c -- runtime-probed microSD mount. See sdcard.h for the contract. */

#include "sdcard.h"

#if defined(TESSERAE_SD_SLOT)

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmic.h"    /* PhotoPainter: the TF slot rides the AXP2101 rails */
#include "sdmmc_cmd.h"

#if defined(SD_USE_SDMMC)
#include "driver/sdmmc_host.h"
#else
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#endif

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;

bool sdcard_mounted(void) { return s_card != NULL; }

void sdcard_quiesce(void)
{
#if !defined(SD_USE_SDMMC)
    /* Shared bus: deselect the card (CS high) and cut the slot rail so a
     * fitted card can never sit half-selected on the panel's SPI lines. The
     * sdspi driver re-owns CS on mount; mount re-raises the rail. */
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << SD_PIN_CS)
#ifdef SD_PIN_EN
                      | (1ULL << SD_PIN_EN)
#endif
        ,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&out);
    gpio_set_level((gpio_num_t)SD_PIN_CS, 1);
#ifdef SD_PIN_EN
    gpio_set_level((gpio_num_t)SD_PIN_EN, 0);
#endif
#endif /* !SD_USE_SDMMC */
}

bool sdcard_mount(void)
{
    if (s_card) return true;

#ifdef BOARD_HAS_PMIC
    /* PhotoPainter: the TF slot is on the AXP2101's ALDO rail group, which
     * may still be down this early in the wake (the panel driver raises it
     * much later). Idempotent + cheap on repeat calls; no-op on non-PMIC. */
    pmic_init();
    pmic_rails_set(true);
    vTaskDelay(pdMS_TO_TICKS(10));   /* rail settle before the probe */
#endif

#ifdef SD_PIN_DET
    /* Card-detect is active low. No card -> skip the whole probe (fast path
     * for cardless devices on every wake). */
    gpio_config_t det = {
        .pin_bit_mask = 1ULL << SD_PIN_DET,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&det);
    if (gpio_get_level((gpio_num_t)SD_PIN_DET) != 0) {
        ESP_LOGD(TAG, "no card detected");
        return false;
    }
#endif
#ifdef SD_PIN_EN
    /* Slot power gate (active high). Give the card a moment on the rail. */
    gpio_config_t en = {
        .pin_bit_mask = 1ULL << SD_PIN_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&en);
    gpio_set_level((gpio_num_t)SD_PIN_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,   /* the card is the USER'S: never format */
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    esp_err_t err;

#if defined(SD_USE_SDMMC)
    /* Dedicated SDMMC pins; 1-bit keeps the pin count and EMI down and is
     * ample for <=1 MB frames (~2 MB/s). */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_MMC_PIN_CLK;
    slot.cmd = (gpio_num_t)SD_MMC_PIN_CMD;
    slot.d0  = (gpio_num_t)SD_MMC_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    err = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
#else
    /* Shared panel bus: initialise it with the panel's own geometry so the
     * panel driver's later spi_bus_initialize() (which tolerates
     * ESP_ERR_INVALID_STATE) inherits a bus that fits full frames. */
    spi_bus_config_t bus = {
        .mosi_io_num = EPD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = EPD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_BUF_BYTES,
    };
    err = spi_bus_initialize(EPD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        goto fail_power;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = EPD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = EPD_SPI_HOST;
    slot.gpio_cs = (gpio_num_t)SD_PIN_CS;
    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mnt, &s_card);
#endif

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s (cache disabled this wake)",
                 esp_err_to_name(err));
        s_card = NULL;
        goto fail_power;
    }
    ESP_LOGI(TAG, "mounted %s (%llu MB, %llu MB free)", SDCARD_MOUNT_POINT,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20,
             sdcard_free_bytes() >> 20);
    return true;

fail_power:
#ifdef SD_PIN_EN
    gpio_set_level((gpio_num_t)SD_PIN_EN, 0);
#endif
    return false;
}

void sdcard_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
#ifdef SD_PIN_EN
    /* Cut slot power for deep sleep (the pin goes hi-Z in sleep; the board's
     * default keeps the slot dark without an active driver). */
    gpio_set_level((gpio_num_t)SD_PIN_EN, 0);
#endif
}

uint64_t sdcard_free_bytes(void)
{
    if (!s_card) return 0;
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_b) != ESP_OK) return 0;
    return free_b;
}

#endif /* TESSERAE_SD_SLOT */
