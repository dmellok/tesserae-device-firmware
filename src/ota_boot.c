/* ota_boot.c -- ESP-IDF rollback-state handling for newly installed images. */

#include "ota_boot.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_boot";

void ota_boot_confirm_if_pending(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL ||
        esp_ota_get_state_partition(running, &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) return;

    /* app_main reached this only after NVS, the network stack, the WiFi driver,
     * persisted Tesserae configuration, and WiFi association all succeeded.
     * These are local checks: server availability never decides boot validity. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "first boot local checks passed; OTA image marked valid");
    } else {
        ESP_LOGE(TAG, "could not mark OTA image valid: %s; rollback on next boot",
                 esp_err_to_name(err));
    }
#endif
}
