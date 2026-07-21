/* ota_install.h -- stream a verified OTA image into the inactive app slot. */
#pragma once

#include "ota_manifest.h"

typedef enum {
    OTA_INSTALL_APPLIED = 0,
    OTA_INSTALL_INVALID_ARGUMENT,
    OTA_INSTALL_LOW_BATTERY,
    OTA_INSTALL_NO_UPDATE_SLOT,
    OTA_INSTALL_IMAGE_TOO_LARGE,
    OTA_INSTALL_NO_MEMORY,
    OTA_INSTALL_HTTP_INIT_FAILED,
    OTA_INSTALL_HTTP_FAILED,
    OTA_INSTALL_HTTP_STATUS,
    OTA_INSTALL_SIZE_MISMATCH,
    OTA_INSTALL_FLASH_WRITE_FAILED,
    OTA_INSTALL_DIGEST_MISMATCH,
    OTA_INSTALL_IMAGE_INVALID,
    OTA_INSTALL_SET_BOOT_FAILED,
} ota_install_result_t;

/* The manifest must already have passed ota_descriptor_check(). On success,
 * the image is complete, hash-matched, accepted by ESP-IDF, and selected for
 * the next boot. The caller owns the final esp_restart(). */
ota_install_result_t ota_install_apply(const ota_manifest_t *manifest);

const char *ota_install_result_name(ota_install_result_t result);
