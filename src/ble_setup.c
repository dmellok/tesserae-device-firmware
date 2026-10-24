#include "ble_setup.h"
#include "memory_diag.h"
#include "led.h"
#include "panel/busy_sleep.h"

#include "sdkconfig.h"

#ifndef CONFIG_BT_NIMBLE_ENABLED

/* Boards whose sdkconfig leaves NimBLE out (no sdkconfig.ble.defaults overlay)
 * define no BOARD_BTN_REFRESH_PIN either, so both entry points -- the AP-portal
 * hold in provisioning.c and the awake-window hold in main.c -- are compiled
 * out. Keep the symbol so main.c links without per-board #ifdefs. */
ble_setup_result_t ble_setup_run(ble_setup_mode_t mode, uint32_t timeout_s)
{
    (void)mode;
    (void)timeout_s;
    return BLE_SETUP_RESULT_ERROR;
}

#else  /* CONFIG_BT_NIMBLE_ENABLED */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "app_config.h"
#include "battery.h"
#include "buttons.h"
#include "maintenance_button.h"
#include "ble_setup_protocol.h"
#include "ble_photo.h"
#include "ble_photo_mode.h"
#include "epd_driver.h"
#include "mbedtls/sha256.h"
#include "net_rest.h"
#include "relay_crypto.h"
#include "rest_config.h"
#include "splash.h"
#include "wifi_manager.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#ifndef BOARD_BTN_REFRESH_PIN
#error "BLE setup requires a board with a Refresh button"
#endif

_Static_assert(TESSERAE_BLE_HARDWARE_CODE != 0,
               "BLE-enabled boards must define TESSERAE_BLE_HARDWARE_CODE");

static const char *TAG = "ble_setup";

#define BIT_DONE           BIT0
#define BIT_HOST_STOPPED   BIT1
#define BIT_WORKER_STOPPED BIT2
#define COMMAND_QUEUE_LEN  4
#define EVENT_FRAME_MAX    300
#define LOG_ROWS           10

#ifndef TESSERAE_BLE_PHOTO_ENABLED
#define TESSERAE_BLE_PHOTO_ENABLED 0
#endif
#define PHOTO_CHUNK_BYTES 384
#define PHOTO_WINDOW_PACKETS COMMAND_QUEUE_LEN
#if TESSERAE_BLE_PHOTO_ENABLED && !defined(TESSERAE_BOARD_SEEED_E1004) && \
    !defined(TESSERAE_BOARD_SEEED_E1001) && !defined(TESSERAE_BOARD_SEEED_STICKY) && \
    !defined(TESSERAE_BOARD_SEEED_E1003)
#error "BLE Photo v2 experimental profiles: E1004, E1001, Sticky, E1003 only"
#endif

/* UUID text: 7a5e000N-7b6d-4f8b-9c2e-1d0a5a110001. NimBLE stores 128-bit
 * UUID bytes least-significant first, hence N belongs at byte 12 rather than
 * byte 0 (which is the trailing 01 in the canonical UUID). */
#define TESSERAE_UUID(suffix) \
    BLE_UUID128_INIT(0x01,0x00,0x11,0x5a,0x0a,0x1d,0x2e,0x9c, \
                     0x8b,0x4f,0x6d,0x7b,(suffix),0x00,0x5e,0x7a)

static const ble_uuid128_t s_service_uuid = TESSERAE_UUID(0x01);
static const ble_uuid128_t s_info_uuid    = TESSERAE_UUID(0x02);
static const ble_uuid128_t s_qr_uuid      = TESSERAE_UUID(0x03);
static const ble_uuid128_t s_secure_uuid  = TESSERAE_UUID(0x04);
static const ble_uuid128_t s_event_uuid   = TESSERAE_UUID(0x05);
#if TESSERAE_BLE_PHOTO_ENABLED
static const ble_uuid128_t s_photo_uuid   = TESSERAE_UUID(0x06);
static const ble_uuid128_t s_photo_service_uuid = TESSERAE_UUID(0x08);
#endif

typedef enum { AUTH_QR = 1, AUTH_PASSKEY = 2 } auth_mode_t;

typedef struct {
    auth_mode_t auth;
    uint32_t generation;
    bool binary;
    size_t len;
    char json[BLE_SETUP_MESSAGE_MAX + 1];
} command_t;

typedef struct {
    char ssid[33];
    char password[65];
    char server_url[160];
    char pairing_code[16];
    bool preserve_server;
    bool ready;
} staged_config_t;

static ble_setup_mode_t s_mode;
static ble_setup_result_t s_result;
static EventGroupHandle_t s_events;
static QueueHandle_t s_commands;
static TaskHandle_t s_worker;
static atomic_bool s_stopping;
static atomic_bool s_notify;
static bool s_nimble_initialized;
static bool s_host_started;
static atomic_ushort s_conn = BLE_HS_CONN_HANDLE_NONE;
/* Mutex protects key/counter replacement across GATT and worker tasks, never
 * Wi-Fi or panel work. Only the worker owns the photo buffer. */
static SemaphoreHandle_t s_session_mutex;
static atomic_uint s_generation;
static uint32_t s_reply_generation;
static uint16_t s_event_handle;
static uint8_t s_addr_type;
static uint32_t s_passkey;
static uint8_t s_secret[BLE_SETUP_KEY_LEN];
static uint8_t s_sid[BLE_SETUP_SID_LEN];
static uint8_t s_connection_nonce[BLE_SETUP_CONN_NONCE_LEN];
static ble_setup_crypto_t s_crypto;
static ble_setup_reassembly_t s_qr_reassembly;
static ble_setup_reassembly_t s_native_reassembly;
#if TESSERAE_BLE_PHOTO_ENABLED
static ble_setup_reassembly_t s_photo_reassembly;
static ble_photo_rx_t s_photo_rx;
static uint8_t s_photo_sha256[32];
static auth_mode_t s_photo_auth;
static atomic_bool s_mode_reboot;
static atomic_int_fast64_t s_photo_last_authenticated;
static char s_photo_capabilities[BLE_SETUP_MESSAGE_MAX + 1];
static char s_photo_digest[65];
static const char *s_photo_format;
#endif
static staged_config_t s_staged;
static uint16_t s_out_message_id;
static char s_info[512];
static char s_qr_payload[320];
static uint8_t s_last_event[EVENT_FRAME_MAX];
static size_t s_last_event_len;
static char s_logs[LOG_ROWS][72];
static size_t s_log_count;

static int gap_event(struct ble_gap_event *event, void *arg);

static const ble_uuid128_t *active_service_uuid(void)
{
#if TESSERAE_BLE_PHOTO_ENABLED
    if (s_mode == BLE_SETUP_MODE_PHOTO) return &s_photo_service_uuid;
#endif
    return &s_service_uuid;
}

static bool prepare_connection(void)
{
    esp_fill_random(s_connection_nonce, sizeof s_connection_nonce);
    if (!ble_setup_crypto_init(&s_crypto, s_secret, s_sid,
                               s_connection_nonce))
        return false;

    char sid_hex[BLE_SETUP_SID_LEN * 2 + 1];
    snprintf(sid_hex, sizeof sid_hex, "%02x%02x%02x%02x",
             s_sid[0], s_sid[1], s_sid[2], s_sid[3]);
    char nonce_hex[BLE_SETUP_CONN_NONCE_LEN * 2 + 1];
    for (size_t i = 0; i < sizeof s_connection_nonce; i++)
        snprintf(nonce_hex + i * 2, sizeof nonce_hex - i * 2, "%02x",
                 s_connection_nonce[i]);
    const char *device_id = rest_config_device_id();
    int n = snprintf(s_info, sizeof s_info,
                     "{\"protocol\":%u,\"id\":\"%s\",\"sid\":\"%s\","
                     "\"connection_nonce\":\"%s\",\"hardware\":%u,"
                     "\"model\":\"%s\",\"firmware\":\"%s\","
                     "\"mode\":\"%s\"%s}",
                     (unsigned)BLE_SETUP_PROTOCOL_MAJOR, device_id, sid_hex,
                     nonce_hex, (unsigned)TESSERAE_BLE_HARDWARE_CODE,
                     TESSERAE_DEVICE_MODEL, FW_VERSION,
                     s_mode == BLE_SETUP_MODE_NEW_DEVICE ? "setup" :
                     s_mode == BLE_SETUP_MODE_PHOTO ? "photo" : "maintenance",
#if TESSERAE_BLE_PHOTO_ENABLED
                     s_mode == BLE_SETUP_MODE_MAINTENANCE || s_mode == BLE_SETUP_MODE_PHOTO
                        ? ",\"photo\":2,\"family\":\"tesserae\",\"kind\":\"" TESSERAE_DEVICE_KIND "\"" : ""
#else
                     ""
#endif
                     );
    if (n <= 0 || (size_t)n >= sizeof s_info) return false;

    s_out_message_id = 0;
    memset(s_last_event, 0, sizeof s_last_event);
    s_last_event_len = 0;
    ble_setup_reassembly_reset(&s_qr_reassembly);
    ble_setup_reassembly_reset(&s_native_reassembly);
#if TESSERAE_BLE_PHOTO_ENABLED
    ble_setup_reassembly_reset(&s_photo_reassembly);
#endif
    return true;
}

static void add_log(const char *text)
{
    if (!text) return;
    if (s_log_count == LOG_ROWS) {
        memmove(s_logs, s_logs + 1, sizeof s_logs[0] * (LOG_ROWS - 1));
        s_log_count--;
    }
    snprintf(s_logs[s_log_count++], sizeof s_logs[0], "%s", text);
}

static bool session_cancelled(void)
{
    return s_stopping;
}

static void finish(ble_setup_result_t result)
{
    if (session_cancelled()) return;
    s_result = result;
    xEventGroupSetBits(s_events, BIT_DONE);
}

static bool digits_only(const char *value, size_t length)
{
    if (!value || strlen(value) != length) return false;
    for (size_t i = 0; i < length; i++) if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

static void strip_trailing_slashes(char *value)
{
    size_t n = strlen(value);
    while (n > 0 && value[n - 1] == '/') value[--n] = '\0';
}

static bool copy_json_string(const cJSON *root, const char *name,
                             char *out, size_t cap, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return !required;
    size_t n = strlen(item->valuestring);
    if (n >= cap) return false;
    memcpy(out, item->valuestring, n + 1);
    return true;
}

static size_t event_payload_limit(auth_mode_t auth)
{
    uint16_t conn = atomic_load(&s_conn);
    uint16_t mtu = conn == BLE_HS_CONN_HANDLE_NONE ? 23 : ble_att_mtu(conn);
    size_t frame = mtu > 3 ? (size_t)mtu - 3 : 20;
    size_t overhead = 1 + BLE_SETUP_CHUNK_HEADER;
    if (auth == AUTH_QR) overhead += 4 + BLE_SETUP_TAG_LEN;
    return frame > overhead ? frame - overhead : 1;
}

static int notify_frame(const uint8_t *frame, size_t len)
{
    /* The caller holds s_session_mutex. NimBLE invokes GAP/GATT application
     * callbacks without its host lock, so this ordering cannot invert it. */
    if (session_cancelled()) return BLE_HS_ENOTCONN;
    if (len > sizeof s_last_event) return BLE_HS_EMSGSIZE;
    if (s_reply_generation != atomic_load(&s_generation) ||
        s_conn == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;
    memcpy(s_last_event, frame, len);
    s_last_event_len = len;
    if (!s_notify || s_conn == BLE_HS_CONN_HANDLE_NONE) return 0;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)len);
    if (!om) return BLE_HS_ENOMEM;
    int rc = ble_gatts_notify_custom(s_conn, s_event_handle, om);
    return rc;
}

static void send_event(auth_mode_t auth, const char *json)
{
    if (!json) return;
    size_t total = strlen(json);
    size_t limit = event_payload_limit(auth);
    uint8_t count = (uint8_t)((total + limit - 1) / limit);
    if (count == 0) count = 1;
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_reply_generation != atomic_load(&s_generation) ||
        s_conn == BLE_HS_CONN_HANDLE_NONE) {
        xSemaphoreGive(s_session_mutex);
        return;
    }
    uint16_t message_id = ++s_out_message_id;
    xSemaphoreGive(s_session_mutex);
    for (uint8_t index = 0; index < count; index++) {
        size_t offset = (size_t)index * limit;
        size_t part = total > offset ? total - offset : 0;
        if (part > limit) part = limit;
        uint8_t plain[EVENT_FRAME_MAX], frame[EVENT_FRAME_MAX];
        size_t plain_len = 0, frame_len = 0;
        if (!ble_setup_chunk_encode(message_id, index, count,
                                    (const uint8_t *)json + offset, part,
                                    plain, sizeof plain, &plain_len)) return;
        /* Backpressure only when NimBLE cannot enqueue a notification. A fixed
         * sleep after every Photo ACK serializes an otherwise bounded window.
         * Retries are bounded and re-sealed with fresh counters. Never hold the
         * session mutex while waiting for NimBLE to drain its buffers. */
        const int64_t deadline = esp_timer_get_time() + 100000;
        for (;;) {
            xSemaphoreTake(s_session_mutex, portMAX_DELAY);
            if (s_reply_generation != atomic_load(&s_generation) ||
                s_conn == BLE_HS_CONN_HANDLE_NONE) {
                xSemaphoreGive(s_session_mutex);
                return;
            }
            if (auth == AUTH_QR) {
                if (!ble_setup_seal(&s_crypto, BLE_SETUP_DIR_DEVICE_TO_APP,
                                    plain, plain_len, frame, sizeof frame, &frame_len)) {
                    xSemaphoreGive(s_session_mutex);
                    return;
                }
            } else {
                frame[0] = BLE_SETUP_FRAME_NATIVE;
                memcpy(frame + 1, plain, plain_len);
                frame_len = plain_len + 1;
            }
            int rc = notify_frame(frame, frame_len);
            xSemaphoreGive(s_session_mutex);
            if (rc == 0) break;
            if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) ||
                esp_timer_get_time() >= deadline) {
                ESP_LOGW(TAG, "notify failed: %d", rc);
                return; /* cumulative ACK/retry recovers a lost Photo event */
            }
            vTaskDelay(1);
        }
        if (s_mode != BLE_SETUP_MODE_PHOTO) vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void send_simple(auth_mode_t auth, const char *event, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", event);
    if (message) cJSON_AddStringToObject(root, "message", message);
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded) { send_event(auth, encoded); free(encoded); }
}

/* Use the same board-specific state of charge as the network heartbeat,
 * including the Sticky fuel gauge. Missing sensing is unknown, never 0%. */
static void add_battery_status(cJSON *root)
{
    bool present = battery_present();
    int mv = present ? battery_read_mv() : 0;
    int pct = present && mv > 0 ? battery_read_pct() : -1;
    if (mv > 0) cJSON_AddNumberToObject(root, "battery_mv", mv);
    else cJSON_AddNullToObject(root, "battery_mv");
    if (pct >= 0 && pct <= 100) cJSON_AddNumberToObject(root, "battery_pct", pct);
    else cJSON_AddNullToObject(root, "battery_pct");
}

static void send_diagnostics(auth_mode_t auth)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", "diagnostics");
    cJSON_AddStringToObject(root, "firmware", FW_VERSION);
    cJSON_AddStringToObject(root, "model", TESSERAE_DEVICE_MODEL);
#if TESSERAE_BLE_PHOTO_ENABLED
    bool manual = false;
    uint8_t photo_key[32];
    if (ble_photo_mode_get(&manual, photo_key) == ESP_OK)
        cJSON_AddStringToObject(root, "screen_mode", manual ? "bluetooth" : "wifi");
    else
        cJSON_AddBoolToObject(root, "screen_mode_error", true);
    memset(photo_key, 0, sizeof photo_key);
#endif
    add_battery_status(root);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    cJSON_AddBoolToObject(root, "wifi_configured", wifi_creds_present());
    cJSON_AddBoolToObject(root, "server_configured", rest_config_has_server());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    char ssid[33] = {0}, ip[48] = {0};
    if (wifi_creds_get_ssid(ssid, sizeof ssid)) cJSON_AddStringToObject(root, "ssid", ssid);
    else cJSON_AddNullToObject(root, "ssid");
    if (wifi_manager_get_sta_ip(ip, sizeof ip)) cJSON_AddStringToObject(root, "ip", ip);
    else cJSON_AddNullToObject(root, "ip");
    cJSON *logs = cJSON_AddArrayToObject(root, "logs");
    for (size_t i = 0; logs && i < s_log_count; i++)
        cJSON_AddItemToArray(logs, cJSON_CreateString(s_logs[i]));
    char *encoded = cJSON_PrintUnformatted(root);
    /* Diagnostic history is optional. Keep the control reply within the
     * shared reassembly bound, including the Photo v2 screen_mode field. */
    while (encoded && strlen(encoded) > BLE_SETUP_MESSAGE_MAX &&
           logs && cJSON_GetArraySize(logs) > 0) {
        free(encoded);
        cJSON_DeleteItemFromArray(logs, 0);
        encoded = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (encoded) { send_event(auth, encoded); free(encoded); }
}

static void run_scan(auth_mode_t auth)
{
    send_simple(auth, "scan_started", NULL);
    wifi_network_t networks[WIFI_SCAN_MAX_NETWORKS];
    size_t count = 0;
    esp_err_t err = wifi_scan_networks(networks, WIFI_SCAN_MAX_NETWORKS, &count);
    if (err != ESP_OK) {
        add_log("Wi-Fi scan failed");
        send_simple(auth, "error", "Wi-Fi scan failed");
        return;
    }
    char logline[72];
    snprintf(logline, sizeof logline, "Wi-Fi scan found %u networks", (unsigned)count);
    add_log(logline);
    for (size_t i = 0; i < count; i++) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event", "network");
        cJSON_AddStringToObject(root, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(root, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(root, "secure", networks[i].secure);
        char *encoded = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (encoded) { send_event(auth, encoded); free(encoded); }
    }
    send_simple(auth, "scan_complete", NULL);
}

static void stage_config(auth_mode_t auth, const cJSON *root)
{
    staged_config_t next = {0};
    if (!copy_json_string(root, "ssid", next.ssid, sizeof next.ssid, true) ||
        !copy_json_string(root, "password", next.password, sizeof next.password, true) ||
        !copy_json_string(root, "server_url", next.server_url, sizeof next.server_url, true) ||
        !copy_json_string(root, "pairing_code", next.pairing_code,
                          sizeof next.pairing_code, false) ||
        !next.ssid[0]) {
        send_simple(auth, "error", "Invalid setup fields");
        return;
    }
    strip_trailing_slashes(next.server_url);
    if (!next.server_url[0]) {
        const rest_config_t *current = rest_config_get();
        if (s_mode != BLE_SETUP_MODE_MAINTENANCE || !current->server_url[0]) {
            send_simple(auth, "error", "A Tesserae server URL is required");
            return;
        }
        snprintf(next.server_url, sizeof next.server_url, "%s", current->server_url);
        next.preserve_server = true;
    }
    if ((strncmp(next.server_url, "http://", 7) != 0 &&
         strncmp(next.server_url, "https://", 8) != 0) ||
        (next.pairing_code[0] && !digits_only(next.pairing_code, 6))) {
        send_simple(auth, "error", "Invalid server URL or pairing code");
        return;
    }
    bool server_changed = strcmp(rest_config_get()->server_url, next.server_url) != 0;
    if (!next.preserve_server &&
        (server_changed || !rest_config_get()->device_token[0]) &&
        !next.pairing_code[0]) {
        send_simple(auth, "error", "A device pairing code is required");
        return;
    }
    next.ready = true;
    s_staged = next;
    add_log("Configuration staged in memory");
    send_simple(auth, "staged", NULL);
}

static void apply_config(auth_mode_t auth)
{
    if (!s_staged.ready) {
        send_simple(auth, "error", "Stage configuration first");
        return;
    }
    send_simple(auth, "testing_wifi", NULL);
    esp_err_t err = wifi_sta_connect_credentials_cancellable(
        s_staged.ssid, s_staged.password, session_cancelled);
    if (session_cancelled()) return;
    if (err != ESP_OK) {
        add_log("Staged Wi-Fi connection failed");
        send_simple(auth, "wifi_failed", "Could not connect to that Wi-Fi network");
        return;
    }
    add_log("Staged Wi-Fi connected");
    send_simple(auth, "wifi_connected", NULL);
    send_simple(auth, "testing_server", NULL);
    bool server_ok = rest_probe_server_url(s_staged.server_url, 10000);
    if (session_cancelled()) return;
    if (!server_ok) {
        add_log("Staged Tesserae server check failed");
        wifi_sta_stop();
        send_simple(auth, "server_failed", "Could not verify that Tesserae server");
        return;
    }
    add_log("Staged Tesserae server verified");
    send_simple(auth, "server_connected", NULL);

    bool server_changed = strcmp(rest_config_get()->server_url, s_staged.server_url) != 0;
    err = wifi_creds_save(s_staged.ssid, s_staged.password);
    if (err == ESP_OK) {
        if (!s_staged.preserve_server)
            rest_config_set_server(s_staged.server_url);
        if (s_staged.pairing_code[0]) rest_config_set_pairing(s_staged.pairing_code);
        if (server_changed || s_staged.pairing_code[0]) {
            rest_config_set_device_token("");
            rest_config_set_frame_etag("");
            rest_config_set_ui_state(0);
        }
        rest_config_clear_relay();
        err = rest_config_save();
    }
    if (err != ESP_OK) {
        add_log("Configuration persistence failed");
        send_simple(auth, "error", "Could not save configuration");
        return;
    }
    add_log("Configuration committed");
    send_simple(auth, "configured", "Restarting display");
    vTaskDelay(pdMS_TO_TICKS(250));
    finish(BLE_SETUP_RESULT_CONFIGURED);
}

#if TESSERAE_BLE_PHOTO_ENABLED
static void photo_discard(void)
{
    free(s_photo_rx.buffer);
    ble_photo_cancel(&s_photo_rx);
    memset(s_photo_sha256, 0, sizeof s_photo_sha256);
}

static void hex32(const uint8_t digest[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[2 * i + 1] = digits[digest[i] & 15];
    }
    out[64] = '\0';
}

static bool parse_hex32(const char *hex, uint8_t out[32])
{
    if (!hex || strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char ch = hex[2 * i + j];
            unsigned nibble;
            if (ch >= '0' && ch <= '9') nibble = (unsigned)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') nibble = (unsigned)(ch - 'a' + 10);
            else return false;
            value = (value << 4) | nibble;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static bool prepare_photo_capabilities(void)
{
    const epd_panel_info_t *info = &epd_active_driver()->info;
#if defined(TESSERAE_BOARD_SEEED_E1004)
    s_photo_format = "spectra6-msb";
    const unsigned expected_bpp = 4;
#elif defined(TESSERAE_BOARD_SEEED_E1001)
    s_photo_format = "mono1-msb";
    const unsigned expected_bpp = 1;
#elif defined(TESSERAE_BOARD_SEEED_E1003)
    s_photo_format = "gray4-msb";
    const unsigned expected_bpp = 4;
#else
    s_photo_format = "gray2-msb";
    const unsigned expected_bpp = 2;
#endif
    size_t stride = ((size_t)info->width * info->bpp + 7) / 8;
    if (!info->width || !info->height || info->bpp != expected_bpp ||
        stride * info->height != info->buf_bytes ||
        info->buf_bytes > 4U * 1024U * 1024U) return false;
    /* Canonical UTF-8 string, no newline. Field order is part of Photo v2.0.
     * See docs/ble-photo-v2.md; the device computes this, the phone echoes it. */
    char canonical[384];
    int n = snprintf(canonical, sizeof canonical,
                     "2|%s|%u|%u|%u|%u|0|%s|%u|%u|384|top_down|manual",
                     TESSERAE_DEVICE_KIND, info->width, info->height,
                     info->width, info->height, s_photo_format,
                     (unsigned)stride, (unsigned)info->buf_bytes);
    uint8_t hash[32];
    if (n < 0 || (size_t)n >= sizeof canonical ||
        mbedtls_sha256((const uint8_t *)canonical, (size_t)n, hash, 0) != 0) return false;
    hex32(hash, s_photo_digest);
    n = snprintf(s_photo_capabilities, sizeof s_photo_capabilities,
                 "{\"event\":\"photo_capabilities\",\"schema\":2,\"kind\":\"%s\","
                 "\"width\":%u,\"height\":%u,\"visible_width\":%u,\"visible_height\":%u,"
                 "\"x_offset\":0,\"format\":\"%s\",\"stride\":%u,\"bytes\":%u,"
                 "\"chunk_bytes\":384,\"digest\":\"%s\",\"row_order\":\"top_down\","
                 "\"lifecycle\":\"manual\",\"max_window\":%u,"
                 "\"write_without_response\":true}",
                 TESSERAE_DEVICE_KIND, info->width, info->height, info->width,
                 info->height, s_photo_format, (unsigned)stride,
                 (unsigned)info->buf_bytes, s_photo_digest, (unsigned)PHOTO_WINDOW_PACKETS);
    return n > 0 && (size_t)n <= BLE_SETUP_MESSAGE_MAX;
}

static bool json_u32(const cJSON *root, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsNumber(item) ||
        !(item->valuedouble >= 0 && item->valuedouble <= UINT32_MAX)) return false;
    uint32_t value = (uint32_t)item->valuedouble;
    if ((double)value != item->valuedouble) return false;
    *out = value;
    return true;
}

static bool json_matches(const cJSON *root, const char *name, const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, expected) == 0;
}

static void photo_ack(auth_mode_t auth)
{
    char json[96];
    snprintf(json, sizeof json,
             "{\"event\":\"photo_ack\",\"id\":%" PRIu32 ",\"offset\":%u}",
             s_photo_rx.id, (unsigned)s_photo_rx.received);
    send_event(auth, json);
}

static void process_photo_packet(const command_t *command)
{
    if (s_mode != BLE_SETUP_MODE_PHOTO ||
        command->auth != s_photo_auth ||
        (!ble_photo_append(&s_photo_rx, (const uint8_t *)command->json, command->len,
                           PHOTO_CHUNK_BYTES, command->generation) &&
         !ble_photo_is_gap(&s_photo_rx, (const uint8_t *)command->json, command->len,
                           PHOTO_CHUNK_BYTES, command->generation))) {
        send_simple(command->auth, "error", "Invalid photo packet");
        return;
    }
    photo_ack(command->auth);
}

static void process_photo_command(const command_t *command, const cJSON *root, const char *op)
{
    if (s_mode != BLE_SETUP_MODE_PHOTO) {
        send_simple(command->auth, "error", "Enter Bluetooth photo mode first");
        return;
    }
    if (strcmp(op, "photo_capabilities") == 0) {
        cJSON *reply = cJSON_Parse(s_photo_capabilities);
        if (!reply) { send_simple(command->auth, "error", "Could not read photo capabilities"); return; }
        add_battery_status(reply);
        char *encoded = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        if (encoded && strlen(encoded) <= BLE_SETUP_MESSAGE_MAX) send_event(command->auth, encoded);
        else send_simple(command->auth, "error", "Photo capabilities reply too large");
        free(encoded);
        return;
    }
    uint32_t id;
    if (!json_u32(root, "id", &id) || id == 0) {
        send_simple(command->auth, "error", "Invalid photo transfer id");
        return;
    }
    if (strcmp(op, "photo_begin") == 0) {
        uint32_t schema, bytes;
        const cJSON *sha = cJSON_GetObjectItemCaseSensitive(root, "sha256");
        uint8_t expected[32];
        if (!json_u32(root, "schema", &schema) || schema != 2 ||
            !json_u32(root, "bytes", &bytes) || bytes != epd_active_driver()->info.buf_bytes ||
            !json_matches(root, "format", s_photo_format) ||
            !json_matches(root, "capability_digest", s_photo_digest) ||
            !cJSON_IsString(sha) || !parse_hex32(sha->valuestring, expected)) {
            send_simple(command->auth, "error", "Photo format or capabilities changed");
            return;
        }
        int mv = battery_read_mv();
        if (mv >= BATTERY_PRESENT_MIN_MV && mv < BATTERY_RESUME_MV) {
            send_simple(command->auth, "error", "Charge this display before sending a photo");
            return;
        }
        photo_discard();
        uint8_t *buffer = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) {
            send_simple(command->auth, "error", "Not enough memory to receive this photo");
            return;
        }
        if (!ble_photo_begin(&s_photo_rx, buffer, bytes, id, command->generation)) {
            free(buffer);
            send_simple(command->auth, "error", "Could not begin this photo");
            return;
        }
        memcpy(s_photo_sha256, expected, sizeof expected);
        s_photo_auth = command->auth;
        /* Disconnect may race allocation, but cannot bind this buffer to a
         * later connection. Its queued work carries the original generation. */
        if (command->generation != atomic_load(&s_generation)) {
            photo_discard();
            return;
        }
        photo_ack(command->auth);
        return;
    }
    if (!s_photo_rx.buffer || s_photo_rx.id != id ||
        s_photo_rx.generation != command->generation || s_photo_auth != command->auth) {
        send_simple(command->auth, "error", "No matching photo transfer");
        return;
    }
    if (strcmp(op, "photo_status") == 0) {
        uint32_t request;
        if (!json_u32(root, "request", &request) || request == 0) {
            send_simple(command->auth, "error", "Invalid photo status request");
            return;
        }
        /* Ordered with binary data on the same worker queue: this reply is a
         * drain barrier before a client refills after a Write Command loss. */
        char json[96];
        snprintf(json, sizeof json,
                 "{\"event\":\"photo_status\",\"id\":%" PRIu32 ",\"request\":%" PRIu32
                 ",\"offset\":%u}", s_photo_rx.id, request, (unsigned)s_photo_rx.received);
        send_event(command->auth, json);
        return;
    }
    if (strcmp(op, "photo_cancel") == 0) {
        photo_discard();
        char json[80];
        snprintf(json, sizeof json, "{\"event\":\"photo_cancelled\",\"id\":%" PRIu32 "}", id);
        send_event(command->auth, json);
        return;
    }
    if (strcmp(op, "photo_end") == 0) {
        uint8_t actual[32];
        if (s_photo_rx.received != s_photo_rx.size ||
            mbedtls_sha256(s_photo_rx.buffer, s_photo_rx.size, actual, 0) != 0) {
            send_simple(command->auth, "error", "Photo is incomplete");
            return;
        }
        /* Atomic with respect to disconnect/reconnect; a valid commit must
         * survive the phone dropping immediately after its final ACK. */
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        bool verified = command->generation == atomic_load(&s_generation) &&
            ble_photo_finish(&s_photo_rx, s_photo_sha256, actual, command->generation);
        xSemaphoreGive(s_session_mutex);
        if (!verified) {
            photo_discard();
            send_simple(command->auth, "error", "Photo checksum failed or session ended");
            return;
        }
        char json[80];
        snprintf(json, sizeof json, "{\"event\":\"photo_received\",\"id\":%" PRIu32 "}", id);
        send_event(command->auth, json);
        vTaskDelay(pdMS_TO_TICKS(200));
        finish(BLE_SETUP_RESULT_PHOTO_RECEIVED);
        return;
    }
    send_simple(command->auth, "error", "Unsupported photo operation");
}

static void set_photo_screen_mode(const command_t *command, const cJSON *root)
{
    uint32_t request;
    if (s_mode != BLE_SETUP_MODE_MAINTENANCE ||
        !json_u32(root, "request", &request) || request == 0 ||
        (!json_matches(root, "value", "bluetooth") && !json_matches(root, "value", "wifi"))) {
        send_simple(command->auth, "error", "Invalid screen mode request");
        return;
    }
    bool manual = json_matches(root, "value", "bluetooth");
    uint8_t candidate[32], authorized[32];
    esp_fill_random(candidate, sizeof candidate);
    esp_err_t err = ble_photo_mode_set(manual, candidate, authorized);
    memset(candidate, 0, sizeof candidate);
    if (err != ESP_OK) {
        memset(authorized, 0, sizeof authorized);
        send_simple(command->auth, "error", "Screen mode could not be saved");
        return;
    }
    /* Keychain can be saved while this authenticated session stays open.
     * Closing/physical exit/deadline then reboots into the committed mode. */
    atomic_store(&s_mode_reboot, true);
    char json[256], key[RELAY_B64_KEY_CAP] = {0};
    bool encoded = !manual || relay_b64url_encode(key, sizeof key, authorized, sizeof authorized);
    memset(authorized, 0, sizeof authorized);
    if (!encoded) {
        send_simple(command->auth, "error", "Mode saved but photo key could not be returned");
        return;
    }
    int n;
    if (manual)
        n = snprintf(json, sizeof json,
                     "{\"event\":\"screen_mode\",\"value\":\"bluetooth\",\"request\":%" PRIu32
                     ",\"photo_key\":\"%s\"}", request, key);
    else
        n = snprintf(json, sizeof json,
                     "{\"event\":\"screen_mode\",\"value\":\"wifi\",\"request\":%" PRIu32 "}", request);
    memset(key, 0, sizeof key);
    if (n > 0 && (size_t)n < sizeof json) send_event(command->auth, json);
    memset(json, 0, sizeof json);
}
#endif

static void process_command(const command_t *command)
{
    if (session_cancelled() || command->generation != atomic_load(&s_generation)) return;
    s_reply_generation = command->generation;
#if TESSERAE_BLE_PHOTO_ENABLED
    if (s_photo_rx.verified) return;
    if (s_mode == BLE_SETUP_MODE_PHOTO)
        atomic_store(&s_photo_last_authenticated, esp_timer_get_time());
    if (command->binary) {
        process_photo_packet(command);
        return;
    }
#endif
    cJSON *root = cJSON_ParseWithLength(command->json, command->len);
    if (!root) { send_simple(command->auth, "error", "Invalid command JSON"); return; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op) || !op->valuestring) {
        cJSON_Delete(root);
        send_simple(command->auth, "error", "Missing operation");
        return;
    }
    ESP_LOGI(TAG, "command: %s", op->valuestring);
#if TESSERAE_BLE_PHOTO_ENABLED
    if (atomic_load(&s_mode_reboot) && !ble_photo_mode_pending_allows(op->valuestring)) {
        /* Disconnect now means reboot into the committed mode. Do not ACK
         * another deferred action that that reboot could silently replace. */
        send_simple(command->auth, "error", "Close Maintenance before another action");
        cJSON_Delete(root);
        return;
    }
    if (strncmp(op->valuestring, "photo_", 6) == 0) {
        process_photo_command(command, root, op->valuestring);
        cJSON_Delete(root);
        return;
    }
    if (s_mode == BLE_SETUP_MODE_PHOTO) {
        send_simple(command->auth, "error", "Only photo operations are allowed");
        cJSON_Delete(root);
        return;
    }
    if (strcmp(op->valuestring, "set_screen_mode") == 0) {
        set_photo_screen_mode(command, root);
        cJSON_Delete(root);
        return;
    }
    if (s_photo_rx.buffer) {
        send_simple(command->auth, "error", "Finish or cancel the photo first");
        cJSON_Delete(root);
        return;
    }
#endif
    if (strcmp(op->valuestring, "scan") == 0) run_scan(command->auth);
    else if (strcmp(op->valuestring, "stage") == 0) stage_config(command->auth, root);
    else if (strcmp(op->valuestring, "apply") == 0) apply_config(command->auth);
    else if (strcmp(op->valuestring, "diagnostics") == 0) send_diagnostics(command->auth);
    else if (strcmp(op->valuestring, "reboot") == 0) {
        send_simple(command->auth, "rebooting", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_REBOOT);
    } else if (strcmp(op->valuestring, "clear_wifi") == 0) {
        send_simple(command->auth, "clearing_wifi", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_CLEAR_WIFI);
    } else if (strcmp(op->valuestring, "factory_reset") == 0) {
        send_simple(command->auth, "factory_resetting", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_FACTORY_RESET);
    } else send_simple(command->auth, "error", "Unsupported operation");
    cJSON_Delete(root);
}

static void worker_task(void *arg)
{
    (void)arg;
    command_t command;

    /* Maintenance is entered before the ordinary wake cycle connects STA.
     * Bring up the saved network here so the queued diagnostics request can
     * report a live DHCP address and RSSI instead of only the stored SSID. */
    bool allow_saved_wifi = s_mode == BLE_SETUP_MODE_MAINTENANCE;
#if TESSERAE_BLE_PHOTO_ENABLED
    bool manual = false;
    uint8_t key[32];
    if (ble_photo_mode_get(&manual, key) != ESP_OK || manual) allow_saved_wifi = false;
    memset(key, 0, sizeof key);
#endif
    if (allow_saved_wifi && wifi_creds_present()) {
        add_log("Connecting saved Wi-Fi");
        esp_err_t err = wifi_sta_connect_stored_cancellable(session_cancelled);
        if (err == ESP_OK) {
            char ip[48] = {0};
            wifi_manager_get_sta_ip(ip, sizeof ip);
            char line[72];
            snprintf(line, sizeof line, "Wi-Fi connected%s%s",
                     ip[0] ? ": " : "", ip);
            add_log(line);
        } else {
            add_log("Saved Wi-Fi connection failed");
        }
    }

    while (!s_stopping) {
#if TESSERAE_BLE_PHOTO_ENABLED
        if (s_photo_rx.buffer && !s_photo_rx.verified &&
            s_photo_rx.generation != atomic_load(&s_generation)) photo_discard();
#endif
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(250)) == pdTRUE)
            process_command(&command);
        memset(&command, 0, sizeof command);
    }
    xEventGroupSetBits(s_events, BIT_WORKER_STOPPED);
    vTaskDelete(NULL);
}

static int accept_frame(auth_mode_t auth, struct os_mbuf *om, bool binary)
{
    size_t frame_len = OS_MBUF_PKTLEN(om);
    uint8_t frame[BLE_SETUP_MESSAGE_MAX + 32];
    if (frame_len == 0 || frame_len > sizeof frame ||
        os_mbuf_copydata(om, 0, (int)frame_len, frame) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t plain[BLE_SETUP_MESSAGE_MAX + 8]; size_t plain_len = 0;
    uint32_t generation = atomic_load(&s_generation);
    ble_setup_reassembly_t *reassembly = NULL;
    if (auth == AUTH_QR) {
        if (!ble_setup_open(&s_crypto, BLE_SETUP_DIR_APP_TO_DEVICE,
                            frame, frame_len, plain, sizeof plain, &plain_len))
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        reassembly = &s_qr_reassembly;
    } else {
        /* Defense in depth for the native-passkey path: GATT WRITE_ENC |
         * WRITE_AUTHEN already enforces this today. Keep the check at the
         * shared reassembly boundary so a future characteristic/flag change
         * cannot accidentally admit unauthenticated maintenance commands. */
        struct ble_gap_conn_desc connection;
        if (s_conn == BLE_HS_CONN_HANDLE_NONE ||
            ble_gap_conn_find(s_conn, &connection) != 0 ||
            !connection.sec_state.encrypted || !connection.sec_state.authenticated)
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        if (frame[0] != BLE_SETUP_FRAME_NATIVE || frame_len < 2)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(plain, frame + 1, frame_len - 1);
        plain_len = frame_len - 1;
        reassembly = &s_native_reassembly;
    }

#if TESSERAE_BLE_PHOTO_ENABLED
    if (binary) reassembly = &s_photo_reassembly;
#else
    if (binary) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
#endif
    ble_setup_chunk_t chunk;
    if (!ble_setup_chunk_decode(plain, plain_len, &chunk))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    bool valid = false;
    bool complete = ble_setup_reassembly_push(reassembly, &chunk, &valid);
    if (!valid) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (complete) {
        command_t command = { .auth = auth, .generation = generation,
                              .binary = binary, .len = reassembly->len };
        memcpy(command.json, reassembly->bytes, reassembly->len + 1);
        ble_setup_reassembly_reset(reassembly);
        if (xQueueSend(s_commands, &command, 0) != pdTRUE)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uintptr_t which = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && which == 1) {
        int rc = os_mbuf_append(ctxt->om, s_info, strlen(s_info));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && which == 4) {
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        int rc = os_mbuf_append(ctxt->om, s_last_event, s_last_event_len);
        xSemaphoreGive(s_session_mutex);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 2)
    {
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        int rc = accept_frame(AUTH_QR, ctxt->om, false);
        xSemaphoreGive(s_session_mutex);
        return rc;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 3)
    {
        if (s_mode == BLE_SETUP_MODE_PHOTO) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        int rc = accept_frame(AUTH_PASSKEY, ctxt->om, false);
        xSemaphoreGive(s_session_mutex);
        return rc;
    }
#if TESSERAE_BLE_PHOTO_ENABLED
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 5) {
        uint8_t prefix = 0;
        if (s_mode != BLE_SETUP_MODE_PHOTO ||
            os_mbuf_copydata(ctxt->om, 0, 1, &prefix) != 0)
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        auth_mode_t auth = AUTH_QR;
        if (prefix != BLE_SETUP_FRAME_QR) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        int rc = accept_frame(auth, ctxt->om, true);
        xSemaphoreGive(s_session_mutex);
        return rc;
    }
#endif
    (void)conn_handle; (void)attr_handle;
    return BLE_ATT_ERR_UNLIKELY;
}

static struct ble_gatt_svc_def s_services[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {{
        .uuid = &s_info_uuid.u, .access_cb = gatt_access, .arg = (void *)1,
        .flags = BLE_GATT_CHR_F_READ,
    }, {
        .uuid = &s_qr_uuid.u, .access_cb = gatt_access, .arg = (void *)2,
        .flags = BLE_GATT_CHR_F_WRITE,
    }, {
        .uuid = &s_secure_uuid.u, .access_cb = gatt_access, .arg = (void *)3,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    }, {
        .uuid = &s_event_uuid.u, .access_cb = gatt_access, .arg = (void *)4,
        .val_handle = &s_event_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
#if TESSERAE_BLE_PHOTO_ENABLED
    {
        .uuid = &s_photo_uuid.u, .access_cb = gatt_access, .arg = (void *)5,
        /* Photo mode uses only the QR AES-GCM envelope and stored photo key. */
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
#endif
    {0}}
}, {0}};

static void advertise(void)
{
    /* UUID (16) + payload (10) + AD overhead (2) + flags AD (3) exactly fills
     * the 31-byte legacy advertisement without moving identity into the scan
     * response. See docs/ble-setup-protocol.md. */
    uint8_t service_data[26];
    memcpy(service_data, active_service_uuid()->value, 16);
    service_data[16] = BLE_SETUP_PROTOCOL_MAJOR;
    service_data[17] = s_mode == BLE_SETUP_MODE_PHOTO ? 0x04 :
                       s_mode == BLE_SETUP_MODE_NEW_DEVICE ? 0x01 : 0x02;
    service_data[18] = TESSERAE_BLE_HARDWARE_CODE;
    uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(service_data + 19, mac + 3, 3);
    memcpy(service_data + 22, s_sid, sizeof s_sid);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.svc_data_uuid128 = service_data;
    fields.svc_data_uuid128_len = sizeof service_data;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv data failed: %d", rc); return; }

    /* The primary packet is nearly full with authenticated session metadata.
     * Put the discoverable service UUID in the scan response so iOS can use a
     * service-filtered scan, alongside a deliberately short local name. */
    char name[16];
    snprintf(name, sizeof name, "Tes-%02X%02X%02X", mac[3], mac[4], mac[5]);
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = active_service_uuid();
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    response.name = (const uint8_t *)name;
    response.name_len = (uint8_t)strlen(name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) { ESP_LOGE(TAG, "scan response failed: %d", rc); return; }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "advertise failed: %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) advertise();
    else ESP_LOGE(TAG, "address inference failed: %d", rc);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();

    /* The task that owns NimBLE must remain alive long enough to report that
     * the host loop has exited. The main setup task then deletes this suspended
     * task through nimble_port_freertos_deinit() before deinitializing NimBLE.
     * Calling freertos_deinit() here deletes the current task, so statements
     * after it never execute and the old teardown raced a live host task. */
    xEventGroupSetBits(s_events, BIT_HOST_STOPPED);
    vTaskSuspend(NULL);
}

static void log_photo_link(uint16_t conn)
{
    if (s_mode != BLE_SETUP_MODE_PHOTO) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    ESP_LOGI(TAG, "photo link: interval=%u.%02u ms latency=%u timeout=%u ms mtu=%u",
             (unsigned)(desc.conn_itvl * 5 / 4), (unsigned)((desc.conn_itvl * 25) % 100),
             (unsigned)desc.conn_latency, (unsigned)desc.supervision_timeout * 10,
             (unsigned)ble_att_mtu(conn));
}

static void request_photo_link(uint16_t conn)
{
    if (s_mode != BLE_SETUP_MODE_PHOTO) return;
    /* 15–30 ms, zero peripheral latency, 4 s supervision timeout. This is a
     * preference, not a throughput guarantee; the central may decline it. */
    const struct ble_gap_upd_params params = {
        .itvl_min = 12, .itvl_max = 24, .latency = 0,
        .supervision_timeout = 400,
    };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0) ESP_LOGI(TAG, "photo connection preference not applied: %d", rc);
    log_photo_link(conn);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            xSemaphoreTake(s_session_mutex, portMAX_DELAY);
            atomic_fetch_add(&s_generation, 1);
            s_conn = event->connect.conn_handle;
            UBaseType_t stack_before = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "preparing connection crypto (host stack low-watermark=%u)",
                     (unsigned)stack_before);
            bool prepared = prepare_connection();
            xSemaphoreGive(s_session_mutex);
            if (!prepared) {
                ESP_LOGE(TAG, "could not prepare BLE connection crypto");
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                finish(BLE_SETUP_RESULT_ERROR);
                return 0;
            }
            UBaseType_t stack_after = uxTaskGetStackHighWaterMark(NULL);
            add_log("Phone connected over BLE");
            ESP_LOGI(TAG,
                     "phone connected over BLE (handle=%u, host stack low-watermark=%u)",
                     s_conn, (unsigned)stack_after);
            request_photo_link(s_conn);
        } else {
            ESP_LOGW(TAG, "BLE connection failed (status=%d)", event->connect.status);
            if (!s_stopping) advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected from BLE (reason=%d)",
                 event->disconnect.reason);
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        atomic_fetch_add(&s_generation, 1);
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_notify = false;
        /* Native-mode events rely on the encrypted BLE link. Never leave the
         * final plaintext event readable to a later unauthenticated connection. */
        memset(s_last_event, 0, sizeof s_last_event);
        s_last_event_len = 0;
        memset(s_info, 0, sizeof s_info);
        memset(s_connection_nonce, 0, sizeof s_connection_nonce);
        memset(&s_crypto, 0, sizeof s_crypto);
        ble_setup_reassembly_reset(&s_qr_reassembly);
        ble_setup_reassembly_reset(&s_native_reassembly);
#if TESSERAE_BLE_PHOTO_ENABLED
        ble_setup_reassembly_reset(&s_photo_reassembly);
#endif
        xSemaphoreGive(s_session_mutex);
#if TESSERAE_BLE_PHOTO_ENABLED
        if (atomic_load(&s_mode_reboot)) {
            finish(BLE_SETUP_RESULT_REBOOT);
            return 0;
        }
#endif
        if (!s_stopping) advertise();
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (s_mode == BLE_SETUP_MODE_PHOTO) {
            if (event->conn_update.status != 0)
                ESP_LOGI(TAG, "photo connection update declined: %d", event->conn_update.status);
            log_photo_link(event->conn_update.conn_handle);
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        log_photo_link(event->mtu.conn_handle);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_event_handle)
            s_notify = event->subscribe.cur_notify;
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action != BLE_SM_IOACT_DISP) return 0;
        struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = s_passkey };
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static bool prepare_session(void)
{
#if TESSERAE_BLE_PHOTO_ENABLED
    if (s_mode == BLE_SETUP_MODE_PHOTO) {
        bool manual = false;
        if (ble_photo_mode_get(&manual, s_secret) != ESP_OK || !manual) return false;
    } else
#endif
    esp_fill_random(s_secret, sizeof s_secret);
    esp_fill_random(s_sid, sizeof s_sid);
    s_passkey = 100000u + esp_random() % 900000u;
#if TESSERAE_BLE_PHOTO_ENABLED
    if (s_mode == BLE_SETUP_MODE_PHOTO) return true; /* never expose its key as a setup QR */
#endif
    char key[RELAY_B64_KEY_CAP];
    if (!relay_b64url_encode(key, sizeof key, s_secret, sizeof s_secret)) return false;
    char sid_hex[9];
    snprintf(sid_hex, sizeof sid_hex, "%02x%02x%02x%02x",
             s_sid[0], s_sid[1], s_sid[2], s_sid[3]);
    const char *device_id = rest_config_device_id();
    int n = snprintf(s_qr_payload, sizeof s_qr_payload,
                     "tesserae://setup?v=%u&id=%s&sid=%s&key=%s",
                     (unsigned)BLE_SETUP_PROTOCOL_MAJOR,
                     device_id, sid_hex, key);
    return n > 0 && (size_t)n < sizeof s_qr_payload;
}

static bool start_ble(void)
{
    memory_diag("before nimble init");
    esp_err_t err = nimble_port_init();
    memory_diag("after nimble init");
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err)); return false; }
    s_nimble_initialized = true;
    s_services[0].uuid = &active_service_uuid()->u;
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_services) != 0 || ble_gatts_add_svcs(s_services) != 0) {
        nimble_port_deinit();
        s_nimble_initialized = false;
        return false;
    }
    ble_svc_gap_device_name_set("Tesserae");
    nimble_port_freertos_init(host_task);
    s_host_started = true;
    memory_diag("BLE host started");
    return true;
}

static void stop_ble(void)
{
    s_stopping = true;

    /* Stop application work before dismantling the GATT host. This prevents a
     * scan or configuration command from sending through NimBLE while its
     * queues and mutexes are being released. */
    if (s_worker) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_WORKER_STOPPED, pdFALSE, pdTRUE,
            /* Allow an in-flight server probe (10 s request timeout) to
             * return before destroying its owning worker/host state. */
            pdMS_TO_TICKS(12000));
        if (!(bits & BIT_WORKER_STOPPED)) {
            ESP_LOGE(TAG, "BLE worker did not stop; restarting instead of unsafe teardown");
            esp_restart();
        }
        s_worker = NULL;
    }

    if (s_nimble_initialized && s_host_started) {
        /* nimble_port_stop() already terminates active links and advertising.
         * Manually terminating first raced its asynchronous disconnect with
         * ble_hs_stop(), which returned EALREADY and left the host task live
         * while the caller deinitialized its event queue. */
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "NimBLE host stop failed (%d); restarting safely", rc);
            esp_restart();
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_HOST_STOPPED, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(3000));
        if (!(bits & BIT_HOST_STOPPED)) {
            ESP_LOGE(TAG, "NimBLE host task did not exit; restarting safely");
            esp_restart();
        }

        nimble_port_freertos_deinit();
        s_host_started = false;
    }
    if (s_nimble_initialized) {
        esp_err_t err = nimble_port_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE deinit failed: %s; restarting safely",
                     esp_err_to_name(err));
            esp_restart();
        }
    }
    s_nimble_initialized = false;
    s_host_started = false;
    ESP_LOGI(TAG, "BLE host stopped cleanly");
}

static void scrub_session(void)
{
    if (s_commands) {
        command_t pending;
        while (xQueueReceive(s_commands, &pending, 0) == pdTRUE)
            memset(&pending, 0, sizeof pending);
        vQueueDelete(s_commands);
        s_commands = NULL;
    }
    if (s_events) { vEventGroupDelete(s_events); s_events = NULL; }
    if (s_session_mutex) { vSemaphoreDelete(s_session_mutex); s_session_mutex = NULL; }
#if TESSERAE_BLE_PHOTO_ENABLED
    photo_discard();
#endif
    memset(s_secret, 0, sizeof s_secret);
    memset(s_connection_nonce, 0, sizeof s_connection_nonce);
    memset(&s_crypto, 0, sizeof s_crypto);
    memset(&s_staged, 0, sizeof s_staged);
    memset(&s_qr_reassembly, 0, sizeof s_qr_reassembly);
    memset(&s_native_reassembly, 0, sizeof s_native_reassembly);
    memset(s_qr_payload, 0, sizeof s_qr_payload);
    memset(s_info, 0, sizeof s_info);
    s_passkey = 0;
}

ble_setup_result_t ble_setup_run(ble_setup_mode_t mode, uint32_t timeout_s)
{
    bool button_exit = false;
    memory_diag("BLE session entry");
    epd_light_sleep_set_ble(true);   /* no light sleep under a live BLE host */
    led_blink(100, 1000);            /* slow blink: setup session open */
    s_mode = mode;
    s_result = BLE_SETUP_RESULT_TIMEOUT;
    s_stopping = false; s_notify = false; s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_nimble_initialized = false; s_host_started = false;
    s_worker = NULL; s_out_message_id = 0; s_last_event_len = 0; s_log_count = 0;
    memset(&s_staged, 0, sizeof s_staged);
    ble_setup_reassembly_reset(&s_qr_reassembly);
    ble_setup_reassembly_reset(&s_native_reassembly);
    s_generation = 0;
    s_reply_generation = 0;
#if TESSERAE_BLE_PHOTO_ENABLED
    s_mode_reboot = false;
    s_photo_last_authenticated = 0;
#endif
    s_session_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    if (!s_session_mutex || !s_events || !s_commands || !prepare_session()
#if TESSERAE_BLE_PHOTO_ENABLED
        || !prepare_photo_capabilities()
#endif
        ) {
        scrub_session();
        epd_light_sleep_set_ble(false);
        led_set(false);
        return BLE_SETUP_RESULT_ERROR;
    }
    add_log(mode == BLE_SETUP_MODE_NEW_DEVICE ? "BLE setup started" : "BLE maintenance started");

    /* The E1004 panel refresh holds timing-sensitive SPI/FreeRTOS paths for
     * tens of seconds. Starting the NimBLE host before this paint allowed the
     * two paths to overlap and corrupted a lock while the QR splash was being
     * written, leaving iOS with a stale advertisement and a connection
     * timeout. Make the session visible first, then start the BLE tasks. */
    if (mode != BLE_SETUP_MODE_PHOTO)
        splash_show_ble_setup(s_qr_payload, s_passkey,
                              mode == BLE_SETUP_MODE_MAINTENANCE);
    memory_diag("BLE splash complete");

    if (xTaskCreate(worker_task, "ble_setup_work", 7168, NULL, 5, &s_worker) != pdPASS ||
        !start_ble()) {
        s_stopping = true;
        stop_ble();
        scrub_session();
        epd_light_sleep_set_ble(false);
        led_set(false);
        return BLE_SETUP_RESULT_ERROR;
    }
    buttons_poll_init();
    int64_t started = esp_timer_get_time();
    int64_t deadline = started + (int64_t)timeout_s * 1000000;
    maintenance_button_t button = maintenance_button_init(
        buttons_maintenance_is_pressed(), (uint32_t)(started / 1000));
    while (esp_timer_get_time() < deadline) {
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DONE, pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(20));
        if (bits & BIT_DONE) break;
#if TESSERAE_BLE_PHOTO_ENABLED
        if (mode == BLE_SETUP_MODE_PHOTO) {
            int64_t authenticated = atomic_load(&s_photo_last_authenticated);
            int64_t last = authenticated ? authenticated : started;
            int64_t idle_us = (authenticated ? 300LL : 90LL) * 1000000;
            if (esp_timer_get_time() - last >= idle_us) break;
        }
#endif
        if (maintenance_button_poll(&button, buttons_maintenance_is_pressed(),
                                     (uint32_t)(esp_timer_get_time() / 1000))) {
            ESP_LOGI(TAG, "Refresh released; leaving Bluetooth");
            button_exit = true;
            break;
        }
    }
    stop_ble();
    epd_light_sleep_set_ble(false);
    led_set(false);
    if (button_exit) s_result = BLE_SETUP_RESULT_CANCELLED;
#if TESSERAE_BLE_PHOTO_ENABLED
    if (atomic_load(&s_mode_reboot) &&
        (s_result == BLE_SETUP_RESULT_TIMEOUT || s_result == BLE_SETUP_RESULT_CANCELLED ||
         s_result == BLE_SETUP_RESULT_ERROR)) s_result = BLE_SETUP_RESULT_REBOOT;
#endif
    if (mode == BLE_SETUP_MODE_MAINTENANCE) wifi_sta_stop();
#if TESSERAE_BLE_PHOTO_ENABLED
    /* A verified commit survives an immediate disconnect/deadline racing its
     * final ACK. Both radios are off before the existing driver paints. */
    if (s_photo_rx.verified) {
        /* Photo sessions deliberately do not paint a setup splash, so this
         * boot has not necessarily initialized the panel's SPI/GPIO port. */
        esp_err_t panel_err = epd_port_init();
        if (panel_err == ESP_OK) {
            epd_init();
            epd_display_frame(s_photo_rx.buffer, s_photo_rx.size);
            epd_sleep();
            s_result = BLE_SETUP_RESULT_PHOTO_RECEIVED;
        } else {
            ESP_LOGE(TAG, "verified photo could not initialize panel: %s", esp_err_to_name(panel_err));
            s_result = BLE_SETUP_RESULT_ERROR;
        }
    }
#endif
    scrub_session();
    if (mode == BLE_SETUP_MODE_MAINTENANCE &&
        (s_result == BLE_SETUP_RESULT_CANCELLED || s_result == BLE_SETUP_RESULT_TIMEOUT)) {
        // An offline server cannot replace the now-expired authentication QR.
        splash_show_message("Bluetooth closed", "Hold Refresh for 3 seconds to reconnect.");
    }
    return s_result;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
