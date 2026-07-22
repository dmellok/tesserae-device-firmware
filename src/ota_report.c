/* ota_report.c -- OTA lifecycle state reporting. See ota_report.h. */

#include "ota_report.h"

#if TESSERAE_OTA_CAPABILITY_ENABLED

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ota_report";

#define NVS_NS_OTA        "ota"
#define NVS_KEY_PHASE     "phase"
#define NVS_KEY_REASON    "reason"
#define NVS_KEY_TARGET    "target"
#define NVS_KEY_ATTEMPT   "attempt"

typedef struct {
    uint8_t phase;
    char    reason[24];
    char    target_fw[32];
    char    attempt_id[8];
} ota_report_state_t;

static ota_report_state_t s_state;
static bool s_loaded;

static void load_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

static void ensure_loaded(void)
{
    if (s_loaded) return;
    memset(&s_state, 0, sizeof s_state);
    nvs_handle_t h;
    if (nvs_open(NVS_NS_OTA, NVS_READONLY, &h) == ESP_OK) {
        uint8_t p = 0;
        if (nvs_get_u8(h, NVS_KEY_PHASE, &p) == ESP_OK &&
            p <= OTA_REPORT_ROLLED_BACK) s_state.phase = p;
        load_str(h, NVS_KEY_REASON,  s_state.reason,     sizeof s_state.reason);
        load_str(h, NVS_KEY_TARGET,  s_state.target_fw,  sizeof s_state.target_fw);
        load_str(h, NVS_KEY_ATTEMPT, s_state.attempt_id, sizeof s_state.attempt_id);
        nvs_close(h);
    }
    s_loaded = true;
}

static const char *phase_name(uint8_t phase)
{
    switch (phase) {
    case OTA_REPORT_DOWNLOADING:     return "downloading";
    case OTA_REPORT_PENDING_CONFIRM: return "pending_confirm";
    case OTA_REPORT_CONFIRMED:       return "confirmed";
    case OTA_REPORT_REJECTED:        return "rejected";
    case OTA_REPORT_FAILED:          return "failed";
    case OTA_REPORT_ROLLED_BACK:     return "rolled_back";
    default:                         return NULL;   /* idle: report nothing */
    }
}

static void copy_field(char *out, size_t cap, const char *value)
{
    if (value == NULL) value = "";
    size_t n = strlen(value);
    if (n >= cap) n = cap - 1;
    memcpy(out, value, n);
    out[n] = '\0';
}

void ota_report_set(ota_report_phase_t phase, const char *reason,
                    const char *target_fw, const char *attempt_id)
{
    ensure_loaded();
    s_state.phase = (uint8_t)phase;
    copy_field(s_state.reason,     sizeof s_state.reason,     reason);
    copy_field(s_state.target_fw,  sizeof s_state.target_fw,  target_fw);
    copy_field(s_state.attempt_id, sizeof s_state.attempt_id, attempt_id);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_OTA, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGW(TAG, "nvs open: %s", esp_err_to_name(err)); return; }
    err = nvs_set_u8(h, NVS_KEY_PHASE, s_state.phase);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_REASON,  s_state.reason);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_TARGET,  s_state.target_fw);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_ATTEMPT, s_state.attempt_id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs write: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "phase=%s reason='%s' target=%s",
                  phase_name(s_state.phase) ? phase_name(s_state.phase) : "idle",
                  s_state.reason, s_state.target_fw);
}

void ota_report_resolve_boot(const char *running_fw)
{
    ensure_loaded();
    if (s_state.phase != OTA_REPORT_PENDING_CONFIRM) return;
    if (running_fw != NULL && strcmp(running_fw, s_state.target_fw) == 0) {
        ota_report_set(OTA_REPORT_CONFIRMED, "ok",
                       s_state.target_fw, s_state.attempt_id);
    } else {
        /* We selected the new slot and rebooted, but we are not running the
         * target version: the bootloader's rollback gate reverted the slot. */
        ota_report_set(OTA_REPORT_ROLLED_BACK, "boot_failed",
                       s_state.target_fw, s_state.attempt_id);
    }
}

void ota_report_fill(cJSON *ota_obj)
{
    ensure_loaded();
    const char *phase = phase_name(s_state.phase);
    if (ota_obj == NULL || phase == NULL) return;   /* idle: schema alone */
    cJSON_AddStringToObject(ota_obj, "phase", phase);
    if (s_state.reason[0])     cJSON_AddStringToObject(ota_obj, "reason", s_state.reason);
    if (s_state.target_fw[0])  cJSON_AddStringToObject(ota_obj, "target_fw", s_state.target_fw);
    if (s_state.attempt_id[0]) cJSON_AddStringToObject(ota_obj, "attempt_id", s_state.attempt_id);
}

#endif /* TESSERAE_OTA_CAPABILITY_ENABLED */
