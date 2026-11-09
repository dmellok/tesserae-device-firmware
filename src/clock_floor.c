#include "clock_floor.h"

clock_floor_t clock_floor_none(void)
{
    clock_floor_t floor = { .present = false, .epoch = 0 };
    return floor;
}

bool clock_floor_value_ok(int64_t epoch)
{
    return epoch >= CLOCK_FLOOR_MIN_EPOCH && epoch <= CLOCK_FLOOR_MAX_EPOCH;
}

bool clock_floor_accepts(clock_floor_t floor, int64_t candidate)
{
    if (!floor.present) return true;          /* bootstrap: the gate decides */
    if (candidate <= 0) return false;         /* no clock at all */
    return candidate >= floor.epoch - CLOCK_FLOOR_TOLERANCE_S;
}

bool clock_floor_should_advance(clock_floor_t floor, int64_t observed)
{
    if (!clock_floor_value_ok(observed)) return false;   /* never store junk */
    if (!floor.present) return true;                     /* first mark */
    /* Forward only, and not by less than a day: a device that wakes every
     * fifteen minutes must not write on every wake, and a clock that moves
     * backwards must not drag the mark down with it. */
    return observed > floor.epoch + CLOCK_FLOOR_WRITE_EVERY_S;
}

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"

static const char *TAG = "clock_floor";

static bool open_nvs(nvs_open_mode_t mode, nvs_handle_t *out)
{
    const esp_err_t err = nvs_open(NVS_NS_REST, mode, out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

clock_floor_t clock_floor_load(void)
{
    nvs_handle_t h;
    if (!open_nvs(NVS_READONLY, &h)) return clock_floor_none();

    uint64_t stored = 0;
    const esp_err_t err = nvs_get_u64(h, NVS_KEY_CLK_FLOOR, &stored);
    nvs_close(h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* A device that has never seen a verified response. Not an error. */
        return clock_floor_none();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored mark unreadable (%s); treating as absent",
                 esp_err_to_name(err));
        return clock_floor_none();
    }

    const int64_t epoch = (int64_t)stored;
    if (!clock_floor_value_ok(epoch)) {
        /* Corruption, or a value from a clock that was never sane. Say so:
         * falling back to the bootstrap policy is a real change in strength. */
        ESP_LOGW(TAG, "stored mark %lld is not a plausible epoch; ignoring",
                 (long long)epoch);
        return clock_floor_none();
    }

    clock_floor_t floor = { .present = true, .epoch = epoch };
    return floor;
}

bool clock_floor_advance(int64_t observed)
{
    const clock_floor_t floor = clock_floor_load();
    if (!clock_floor_should_advance(floor, observed)) return false;

    nvs_handle_t h;
    if (!open_nvs(NVS_READWRITE, &h)) return false;

    bool written = false;
    const esp_err_t err = nvs_set_u64(h, NVS_KEY_CLK_FLOOR, (uint64_t)observed);
    if (err == ESP_OK && nvs_commit(h) == ESP_OK) {
        written = true;
        ESP_LOGI(TAG, "mark advanced to %lld", (long long)observed);
    } else {
        ESP_LOGW(TAG, "mark write failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
    return written;
}
#endif
