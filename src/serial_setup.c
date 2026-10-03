#include "serial_setup.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "provisioning.h"
#include "relay.h"
#include "rest_config.h"
#include "sdkconfig.h"
#include "setup_fields.h"
#include "wifi_manager.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

static const char *TAG = "serial_setup";

#define SERIAL_SETUP_PROTOCOL 1
#define LINE_MAX 900

/* One JSON line out. The console is shared with the log, so a reader keys on
 * the "tesserae":"setup" prefix and ignores every other line. */
static void emit(const char *json)
{
    printf("%s\n", json);
    fflush(stdout);
}

static void emit_error(const char *message)
{
    char e[256], line[320];
    setup_json_escape(message, e, sizeof e);
    snprintf(line, sizeof line, "{\"tesserae\":\"setup\",\"ok\":false,\"error\":\"%s\"}", e);
    emit(line);
}

static const char *current_mode(const rest_config_t *cfg)
{
    if (cfg->relay_url[0] && (relay_ready() || relay_pairing_pending())) return "relay";
    if (strcmp(cfg->server_url, SETUP_CLOUD_URL) == 0) return "cloud";
    return "local";
}

/* What the board is: enough for a flasher to show the right form and to say
 * whether this panel is already paired somewhere. No secrets: the password,
 * the token and any pairing code stay on the board. */
static void emit_info(const char *event)
{
    const rest_config_t *cfg = rest_config_get();
    char mac[20] = "";
    rest_config_mac(mac, sizeof mac);
    char ssid[33] = "";
    wifi_creds_get_ssid(ssid, sizeof ssid);
    char e_ssid[80], e_server[340], e_relay[340], networks[700];
    setup_json_escape(ssid, e_ssid, sizeof e_ssid);
    setup_json_escape(cfg->server_url, e_server, sizeof e_server);
    setup_json_escape(cfg->relay_url, e_relay, sizeof e_relay);
    provisioning_scan_json(networks, sizeof networks);
    bool paired = cfg->device_token[0] != '\0' || relay_ready();
    char line[1600];
    snprintf(line, sizeof line,
             "{\"tesserae\":\"setup\",\"event\":\"%s\",\"ok\":true,\"protocol\":%d,\"fw\":\"%s\",\"kind\":\"%s\","
             "\"mac\":\"%s\",\"mode\":\"%s\",\"ssid\":\"%s\",\"server_url\":\"%s\",\"relay_url\":\"%s\","
             "\"paired\":%s,\"portal\":%s,\"networks\":%s}",
             event, SERIAL_SETUP_PROTOCOL, FW_VERSION, TESSERAE_DEVICE_KIND, mac, current_mode(cfg),
             e_ssid, e_server, e_relay, paired ? "true" : "false",
             provisioning_portal_open() ? "true" : "false", networks);
    emit(line);
}

static void handle_set(const char *line)
{
    setup_fields_t f;
    memset(&f, 0, sizeof f);
    char mode[12] = "";
    setup_json_get(line, "ssid", f.ssid, sizeof f.ssid);
    f.have_pass = setup_json_get(line, "pass", f.pass, sizeof f.pass) && f.pass[0];
    setup_json_get(line, "mode", mode, sizeof mode);
    if (!setup_mode_parse(mode, &f.mode)) { emit_error("mode must be local, cloud or relay"); return; }
    setup_json_get(line, "server_url", f.server_url, sizeof f.server_url);
    setup_json_get(line, "pairing_code", f.pairing, sizeof f.pairing);
    setup_json_get(line, "relay_url", f.relay_url, sizeof f.relay_url);
    setup_json_get(line, "relay_code", f.relay_code, sizeof f.relay_code);
    char err[200];
    if (!setup_fields_check(&f, RELAY_DEFAULT_URL, err, sizeof err)) { emit_error(err); return; }
    esp_err_t e = provisioning_apply(&f);
    if (e != ESP_OK) { emit_error(e == ESP_ERR_INVALID_STATE ? "could not write the WiFi settings" : "could not write the Tesserae settings"); return; }
    char out[200];
    snprintf(out, sizeof out, "{\"tesserae\":\"setup\",\"ok\":true,\"saved\":true,\"mode\":\"%s\",\"restarting\":true}", setup_mode_name(f.mode));
    emit(out);
    ESP_LOGI(TAG, "settings saved over serial (%s); restarting", setup_mode_name(f.mode));
    /* With the portal open its own loop restarts the board after the save;
     * otherwise this is the only one who knows. Let the reply leave first. */
    vTaskDelay(pdMS_TO_TICKS(400));
    if (!provisioning_notify_saved()) esp_restart();
}

static void serial_setup_task(void *arg)
{
    (void)arg;
    static char line[LINE_MAX];
    emit_info("hello");
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) {
            /* No driver behind stdin, or nothing yet: never spin. */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        char v[16];
        if (!setup_json_get(line, "tesserae", v, sizeof v) || strcmp(v, "setup") != 0) continue;
        char cmd[16] = "";
        setup_json_get(line, "cmd", cmd, sizeof cmd);
        if (strcmp(cmd, "get") == 0) emit_info("info");
        else if (strcmp(cmd, "set") == 0) handle_set(line);
        else if (strcmp(cmd, "restart") == 0) { emit("{\"tesserae\":\"setup\",\"ok\":true,\"restarting\":true}"); vTaskDelay(pdMS_TO_TICKS(300)); esp_restart(); }
        else emit_error("unknown cmd; use get, set or restart");
    }
}

void serial_setup_start(void)
{
    /* stdin only reads through a driver; the ROM console behind it otherwise
     * answers every read with nothing. Output keeps working either way. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) usb_serial_jtag_vfs_use_driver();
    else { ESP_LOGW(TAG, "usb-serial-jtag driver: %s; serial setup off", esp_err_to_name(err)); return; }
#elif CONFIG_ESP_CONSOLE_UART
    esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    else { ESP_LOGW(TAG, "console uart driver: %s; serial setup off", esp_err_to_name(err)); return; }
#else
    ESP_LOGI(TAG, "no console port; serial setup off");
    return;
#endif
    setvbuf(stdin, NULL, _IONBF, 0);
    if (xTaskCreate(serial_setup_task, "serial_setup", 6144, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "could not start the serial setup task");
}
