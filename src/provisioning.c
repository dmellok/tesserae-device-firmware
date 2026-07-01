#include "provisioning.h"
#include "app_config.h"
#include "mqtt_config.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"

static const char *TAG = "portal";

#define BIT_CREDS_SAVED  BIT0
static EventGroupHandle_t s_done;
static TaskHandle_t s_dns_task = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_mdns_up = false;

/* Idle tracker for PROVISION_PORTAL_TIMEOUT_S. Each STA-connect bumps the
 * counter and pushes the deadline out; when the last STA leaves we reset
 * the deadline to now + PROVISION_PORTAL_TIMEOUT_S. A user actively in the
 * captive portal therefore never gets timed out -- only an abandoned AP
 * with no phone joined eventually trips. */
static int        s_ap_clients = 0;
static int64_t    s_idle_deadline_us = 0;
static esp_event_handler_instance_t s_ap_evt_handle = NULL;

/* Pre-scan cache: filled once at portal start (in STA mode, before the AP
 * comes up) and rendered into the form's <select> so the user can click a
 * nearby SSID instead of typing. Bounded; scans rarely turn up more than a
 * handful of unique networks in a residential context. */
#define SCAN_MAX 12
typedef struct {
    char    ssid[33];
    int8_t  rssi;
    bool    secure;
} scan_entry_t;
static scan_entry_t s_scan[SCAN_MAX];
static int          s_scan_count = 0;

/* ---------- minimal wildcard DNS hijack ----------
 *
 * Listens on UDP/53 and answers every A query with our AP IP (192.168.4.1).
 * Phones probe known URLs (captive.apple.com, connectivitycheck.gstatic.com,
 * etc.); routing those to us makes the OS pop up our HTTP form automatically.
 *
 * We don't parse the DNS query body -- we just copy the question section
 * back, set QR=1 / AA=1 / RCODE=0, and append one A-record answer.        */

static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { ESP_LOGE(TAG, "dns sock fail"); vTaskDelete(NULL); }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind fail");
        close(sock); vTaskDelete(NULL);
    }

    /* 192.168.4.1 in network byte order */
    const uint8_t our_ip[4] = {192, 168, 4, 1};

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) continue;   /* DNS header is 12 bytes minimum */

        /* Flip QR (response), AA (authoritative), set ANCOUNT=1 */
        buf[2] = 0x84;   /* QR=1 AA=1 OPCODE=0 */
        buf[3] = 0x00;   /* RCODE=0 */
        buf[6] = 0x00; buf[7] = 0x01;   /* ANCOUNT */
        buf[8] = 0x00; buf[9] = 0x00;   /* NSCOUNT */
        buf[10]= 0x00; buf[11]= 0x00;   /* ARCOUNT */

        /* Append answer: pointer to qname (0xC00C), TYPE A, CLASS IN,
         * TTL=60, RDLENGTH=4, RDATA=192.168.4.1 */
        int p = n;
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        buf[p++] = 0x00; buf[p++] = 0x01;   /* TYPE A */
        buf[p++] = 0x00; buf[p++] = 0x01;   /* CLASS IN */
        buf[p++] = 0x00; buf[p++] = 0x00;   /* TTL hi */
        buf[p++] = 0x00; buf[p++] = 0x3C;   /* TTL = 60s */
        buf[p++] = 0x00; buf[p++] = 0x04;   /* RDLENGTH */
        for (int i = 0; i < 4; i++) buf[p++] = our_ip[i];

        sendto(sock, buf, p, 0, (struct sockaddr *)&src, slen);
    }
}

/* ---------- HTML ----------
 *
 * Inline CSS uses Tesserae's design tokens (accent #0d8c7e, surface #f1f0ec,
 * etc., from dmellok/tesserae). We can't reach out to fetch CSS files in the
 * captive portal so everything ships inline -- ~3 KB total page weight,
 * chunked-sent to keep stack usage modest.
 */
static const char k_head[] =
"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
"<title>Tesserae Setup</title>"
"<style>"
":root{--bg:#f1f0ec;--surface:#fff;--fg:#18181b;--muted:#71706c;"
"--accent:#0d8c7e;--accent-hover:#0a6f63;--accent-soft:#e6f3f1;"
"--border:#e6e5e1;--radius:10px}"
"*{box-sizing:border-box}"
"body{margin:0;padding:24px 16px env(safe-area-inset-bottom);"
"font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Inter,system-ui,sans-serif;"
"font-size:15px;background:var(--bg);color:var(--fg);line-height:1.45;"
"-webkit-text-size-adjust:100%;-webkit-font-smoothing:antialiased}"
"main{max-width:480px;margin:0 auto}"
".brand{display:flex;align-items:center;gap:10px;margin:0 0 6px;"
"font-weight:700;font-size:20px;letter-spacing:-0.015em}"
".brand-mark{width:32px;height:32px;border-radius:8px;"
"background:linear-gradient(135deg,var(--accent),var(--accent-hover));"
"box-shadow:inset 0 1px 0 rgba(255,255,255,.35),"
"0 1px 3px rgba(13,140,126,.35),0 6px 16px rgba(13,140,126,.18)}"
".tag{color:var(--muted);font-size:13px;margin:0 0 18px}"
".card{background:var(--surface);border:1px solid var(--border);"
"border-radius:var(--radius);padding:18px 16px;margin-bottom:14px}"
".card h2{margin:0 0 14px;font-size:11px;text-transform:uppercase;"
"letter-spacing:0.08em;color:var(--muted);font-weight:600}"
".status{display:grid;grid-template-columns:auto 1fr;gap:6px 12px;"
"font-size:13px;color:var(--muted);margin-bottom:14px;padding:14px 16px;"
"background:var(--surface);border:1px solid var(--border);"
"border-radius:var(--radius)}"
".status .k{font-weight:600;color:var(--fg)}"
".status code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"font-size:12px;background:#fafaf9;padding:1px 6px;border-radius:4px;border:1px solid var(--border)}"
".field{margin-bottom:14px}"
".field:last-child{margin-bottom:0}"
"label{display:block;font-weight:500;font-size:13px;margin-bottom:6px;color:var(--fg)}"
"input,select{width:100%;padding:10px 12px;border:1px solid var(--border);"
"border-radius:6px;background:var(--surface);font:inherit;font-size:15px;"
"color:var(--fg);-webkit-appearance:none;appearance:none;"
"transition:border-color .12s,box-shadow .12s}"
"input:focus,select:focus{outline:none;border-color:var(--accent);"
"box-shadow:0 0 0 3px rgba(13,140,126,.18)}"
"select{background-image:linear-gradient(45deg,transparent 50%,var(--muted) 50%),"
"linear-gradient(135deg,var(--muted) 50%,transparent 50%);"
"background-position:calc(100% - 16px) 50%,calc(100% - 11px) 50%;"
"background-size:5px 5px,5px 5px;background-repeat:no-repeat;padding-right:28px;cursor:pointer}"
".pw{position:relative}"
".pw input{padding-right:64px}"
".pw button{position:absolute;right:4px;top:50%;transform:translateY(-50%);"
"background:none;border:0;color:var(--accent);font:inherit;font-size:13px;"
"font-weight:600;padding:8px 10px;cursor:pointer;border-radius:4px}"
".pw button:hover{background:var(--accent-soft)}"
".hint{margin-top:6px;font-size:12px;color:var(--muted)}"
".hint code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;"
"background:#fafaf9;padding:1px 5px;border-radius:3px;border:1px solid var(--border);font-size:11px}"
".err{background:#fef2f2;border:1px solid #fecaca;color:#b91c1c;"
"padding:12px 14px;border-radius:var(--radius);margin-bottom:14px;font-size:14px}"
"button.submit{width:100%;padding:12px 16px;border:0;border-radius:8px;"
"background:var(--accent);color:#fff;font:inherit;font-size:15px;"
"font-weight:600;cursor:pointer;margin-top:4px;"
"transition:background .12s}"
"button.submit:hover{background:var(--accent-hover)}"
"button.submit:active{transform:translateY(1px)}"
"</style></head><body><main>"
"<div class=\"brand\"><span class=\"brand-mark\"></span><span>Tesserae</span></div>"
"<p class=\"tag\">Device setup</p>";

/* WiFi card; %s x2 = (ssid prefill, scan-picker HTML or empty) */
static const char k_form_wifi_fmt[] =
"<form method=\"POST\" action=\"/save\">"
"<section class=\"card\"><h2>WiFi network</h2>"
"<div class=\"field\">"
"<label for=\"ssid\">Network name (SSID) *</label>"
"<input id=\"ssid\" name=\"ssid\" required maxlength=\"32\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"my-home-wifi\">"
"</div>"
"%s"
"<div class=\"field pw\">"
"<label for=\"wifi-pw\">Password</label>"
"<input id=\"wifi-pw\" name=\"pass\" type=\"password\" maxlength=\"64\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"wifi-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>";

/* MQTT card; %s x3 = (mqtt_uri, device_id, mqtt_user) */
static const char k_form_mqtt_fmt[] =
"<section class=\"card\"><h2>MQTT broker</h2>"
"<div class=\"field\">"
"<label for=\"mqtt_uri\">Broker URI *</label>"
"<input id=\"mqtt_uri\" name=\"mqtt_uri\" required maxlength=\"159\" "
"autocomplete=\"off\" value=\"%s\" placeholder=\"mqtt://192.168.1.50:1883\">"
"<p class=\"hint\">Use <code>mqtts://</code> for TLS; scheme is added if omitted.</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"device_id\">Device id</label>"
"<input id=\"device_id\" name=\"device_id\" maxlength=\"32\" "
"pattern=\"[a-z][a-z0-9_-]{1,31}\" autocomplete=\"off\" "
"value=\"%s\" placeholder=\"esp32\">"
"<p class=\"hint\">Topics: <code>tesserae/&lt;id&gt;/frame/bin</code> etc. "
"Default <code>esp32</code> matches the built-in Tesserae kind.</p>"
"</div>"
"<div class=\"field\">"
"<label for=\"mqtt_user\">Username <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"mqtt_user\" name=\"mqtt_user\" maxlength=\"63\" autocomplete=\"off\" value=\"%s\">"
"</div>"
"<div class=\"field pw\">"
"<label for=\"mqtt-pw\">Password <span style=\"color:var(--muted);font-weight:400\">(optional)</span></label>"
"<input id=\"mqtt-pw\" name=\"mqtt_pass\" type=\"password\" maxlength=\"63\" autocomplete=\"off\">"
"<button type=\"button\" data-toggle=\"mqtt-pw\" aria-label=\"Show password\">Show</button>"
"<p class=\"hint\">Leave blank to keep the current password.</p>"
"</div>"
"</section>"
"<button class=\"submit\" type=\"submit\">Save &amp; restart</button>"
"</form>";

/* JS at end: password show/hide + scan-picker click-to-fill. Inline, no
 * dependencies; runs after the DOM is parsed since it's at the bottom. */
static const char k_tail[] =
"<script>"
"document.querySelectorAll('[data-toggle]').forEach(b=>{"
"b.addEventListener('click',()=>{"
"const i=document.getElementById(b.dataset.toggle);"
"const s=i.type==='password';"
"i.type=s?'text':'password';"
"b.textContent=s?'Hide':'Show';"
"});"
"});"
"const pick=document.getElementById('ssid-pick');"
"if(pick){pick.addEventListener('change',e=>{"
"if(e.target.value){document.getElementById('ssid').value=e.target.value;"
"document.getElementById('wifi-pw').focus();}"
"});}"
"</script></main></body></html>";

static const char k_thanks_html[] =
"<!doctype html><html><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>Saved</title>"
"<style>"
"body{margin:0;padding:40px 16px;background:#f1f0ec;color:#18181b;"
"font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;text-align:center}"
".card{max-width:380px;margin:0 auto;background:#fff;border:1px solid #e6e5e1;"
"border-radius:10px;padding:28px 20px}"
"h1{margin:0 0 8px;font-size:20px;color:#0d8c7e}"
"p{margin:0;color:#71706c;font-size:14px;line-height:1.5}"
"</style></head><body>"
"<div class=\"card\"><h1>Saved</h1>"
"<p>Tesserae will reboot and apply the new settings now.</p></div>"
"</body></html>";

/* Escape &, <, >, " for safe interpolation into HTML attribute values. */
static void html_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t o = 0;
    for (const char *p = src; *p; p++) {
        const char *rep;
        switch (*p) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '"': rep = "&quot;"; break;
            default:
                if (o + 1 >= dst_sz) { dst[o] = '\0'; return; }
                dst[o++] = *p;
                continue;
        }
        size_t rl = strlen(rep);
        if (o + rl >= dst_sz) break;
        memcpy(dst + o, rep, rl);
        o += rl;
    }
    dst[o] = '\0';
}

/* URL-decode %xx and '+' in-place. Caller-owned buffer. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = '\0';
}

/* Pull a named field out of x-www-form-urlencoded body into dst. */
static bool form_field(const char *body, const char *key, char *dst, size_t dst_sz)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= dst_sz) len = dst_sz - 1;
            memcpy(dst, v, len);
            dst[len] = '\0';
            url_decode(dst);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

/* ---------- HTTP handlers ---------- */

/* Render the settings form with live NVS values pre-filled and an optional
 * error banner. Sent chunked so we don't need a multi-KB stack buffer. */
static esp_err_t render_form(httpd_req_t *req, const char *error)
{
    mqtt_config_t cfg;
    mqtt_config_load(&cfg);

    char ssid[33] = {0};
    wifi_creds_get_ssid(ssid, sizeof ssid);
    char ip[16] = {0};
    bool have_ip = wifi_manager_get_sta_ip(ip, sizeof ip);

    char e_ssid[160], e_uri[640], e_devid[160], e_user[256];
    html_escape(ssid,          e_ssid,  sizeof e_ssid);
    html_escape(cfg.uri,       e_uri,   sizeof e_uri);
    html_escape(cfg.device_id, e_devid, sizeof e_devid);
    html_escape(cfg.user,      e_user,  sizeof e_user);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, k_head);

    if (error && *error) {
        httpd_resp_sendstr_chunk(req, "<div class=\"err\">");
        httpd_resp_sendstr_chunk(req, error);   /* our own constant strings, safe */
        httpd_resp_sendstr_chunk(req, "</div>");
    }

    /* Status strip: device id + broker + STA IP (or "setup AP" when the
     * captive portal is up and the device has no STA association yet). */
    char status[512];
    snprintf(status, sizeof status,
        "<div class=\"status\">"
        "<span class=\"k\">Device id</span><span><code>%s</code></span>"
        "<span class=\"k\">Broker</span><span><code>%s</code></span>"
        "<span class=\"k\">IP</span><span><code>%s</code></span>"
        "</div>",
        e_devid, e_uri, have_ip ? ip : "(setup AP)");
    httpd_resp_sendstr_chunk(req, status);

    /* Build the scan-picker <select> block from the cached scan results.
     * Empty string if the scan turned up nothing -- the form still renders
     * with the bare SSID input. */
    char picker[1024] = "";
    if (s_scan_count > 0) {
        size_t o = (size_t)snprintf(picker, sizeof picker,
            "<div class=\"field\">"
            "<label for=\"ssid-pick\">Or pick a nearby network</label>"
            "<select id=\"ssid-pick\" autocomplete=\"off\">"
            "<option value=\"\">-- pick a network --</option>");
        for (int i = 0; i < s_scan_count && o + 128 < sizeof picker; i++) {
            char esc[96];
            html_escape(s_scan[i].ssid, esc, sizeof esc);
            int n = snprintf(picker + o, sizeof picker - o,
                "<option value=\"%s\">%s (%d dBm%s)</option>",
                esc, esc, s_scan[i].rssi, s_scan[i].secure ? "" : ", open");
            if (n < 0 || (size_t)n >= sizeof picker - o) break;
            o += (size_t)n;
        }
        if (o + 16 < sizeof picker) {
            snprintf(picker + o, sizeof picker - o, "</select></div>");
        }
    }

    /* form_wifi must hold: ~580 bytes of literal HTML + escaped ssid + picker
     * (up to ~1 KB). 2048 leaves headroom. form_mqtt needs to hold literal +
     * three escaped values; 1800 covers worst case. */
    char form_wifi[2048];
    snprintf(form_wifi, sizeof form_wifi, k_form_wifi_fmt, e_ssid, picker);
    httpd_resp_sendstr_chunk(req, form_wifi);

    char form_mqtt[1800];
    snprintf(form_mqtt, sizeof form_mqtt, k_form_mqtt_fmt, e_uri, e_devid, e_user);
    httpd_resp_sendstr_chunk(req, form_mqtt);

    httpd_resp_sendstr_chunk(req, k_tail);
    httpd_resp_sendstr_chunk(req, NULL);   /* terminate chunked response */
    return ESP_OK;
}

static esp_err_t h_root(httpd_req_t *req)
{
    return render_form(req, NULL);
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    body[total] = '\0';

    char ssid[33] = {0}, wpa_pass[65] = {0};
    char mqtt_uri[160] = {0}, device_id[33] = {0};
    char mqtt_user[64] = {0}, mqtt_pass[64] = {0};

    bool have_ssid  = form_field(body, "ssid",      ssid,      sizeof ssid)      && ssid[0];
    bool have_uri   = form_field(body, "mqtt_uri",  mqtt_uri,  sizeof mqtt_uri)  && mqtt_uri[0];
    bool have_pass  = form_field(body, "pass",      wpa_pass,  sizeof wpa_pass)  && wpa_pass[0];
    form_field(body, "device_id", device_id, sizeof device_id);
    form_field(body, "mqtt_user", mqtt_user, sizeof mqtt_user);
    bool have_mpass = form_field(body, "mqtt_pass", mqtt_pass, sizeof mqtt_pass) && mqtt_pass[0];

    if (!have_ssid) return render_form(req, "WiFi network name (SSID) is required.");
    if (!have_uri)  return render_form(req, "MQTT broker URI is required.");
    if (!device_id[0]) strcpy(device_id, MQTT_DEFAULT_DEVICE_ID);   /* blank -> default */
    if (!mqtt_device_id_valid(device_id)) {
        return render_form(req,
            "Device id must be 2-32 chars: lowercase letters, digits, '-' or '_', "
            "starting with a letter.");
    }

    ESP_LOGI(TAG, "saving ssid='%s' uri='%s' device_id='%s'",
             ssid, mqtt_uri, device_id);

    /* Passwords: a blank field means "keep what's stored" (NULL), so editing
     * just the device_id via the always-on portal doesn't wipe creds. */
    if (wifi_creds_save(ssid, have_pass ? wpa_pass : NULL) != ESP_OK) {
        return render_form(req, "Failed to write WiFi settings to NVS.");
    }
    if (mqtt_config_save(mqtt_uri, device_id, mqtt_user,
                         have_mpass ? mqtt_pass : NULL) != ESP_OK) {
        return render_form(req, "Failed to write MQTT settings to NVS.");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, k_thanks_html, HTTPD_RESP_USE_STRLEN);
    xEventGroupSetBits(s_done, BIT_CREDS_SAVED);
    return ESP_OK;
}

/* Captive-portal catch-all: redirect anything else to "/" so OS probes land
 * on the form. Only registered in AP mode -- in STA mode we don't want to
 * hijack arbitrary LAN requests. */
static esp_err_t h_catchall(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------- lifecycle helpers ---------- */

/* Pre-AP scan: bring up STA briefly to populate s_scan[], then stop so
 * start_ap() can take over the radio. Failures are non-fatal -- the form
 * still works, just without the picker. */
static void do_wifi_scan(void)
{
    /* The STA netif may not exist yet (provisioning is usually called before
     * any STA connect attempt). Create it if missing -- harmless if it does. */
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_create_default_wifi_sta();
    }

    s_scan_count = 0;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan set_mode: %s", esp_err_to_name(err)); return; }
    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan wifi_start: %s", esp_err_to_name(err)); return; }

    wifi_scan_config_t cfg = {0};
    err = esp_wifi_scan_start(&cfg, /* block */ true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(err));
        esp_wifi_stop();
        return;
    }

    uint16_t n = SCAN_MAX;
    wifi_ap_record_t records[SCAN_MAX];
    esp_wifi_scan_get_ap_records(&n, records);

    for (int i = 0; i < (int)n && s_scan_count < SCAN_MAX; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) continue;   /* skip hidden */
        /* Dedupe: same SSID on multiple APs (mesh / band-steering) collapses
         * to the strongest seen entry, since esp_wifi orders by RSSI desc. */
        bool dup = false;
        for (int j = 0; j < s_scan_count; j++) {
            if (strcmp(s_scan[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(s_scan[s_scan_count].ssid, ssid, 32);
        s_scan[s_scan_count].ssid[32] = '\0';
        s_scan[s_scan_count].rssi = records[i].rssi;
        s_scan[s_scan_count].secure = (records[i].authmode != WIFI_AUTH_OPEN);
        s_scan_count++;
    }
    ESP_LOGI(TAG, "scan: %d unique nearby networks", s_scan_count);

    esp_wifi_stop();
}

static void start_ap(void)
{
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wc.ap.ssid,     PROVISION_AP_SSID, sizeof(wc.ap.ssid) - 1);
    strncpy((char *)wc.ap.password, PROVISION_AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.ssid_len = strlen(PROVISION_AP_SSID);
    if (strlen(PROVISION_AP_PASS) < 8) wc.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP up: ssid=%s ip=192.168.4.1", PROVISION_AP_SSID);
}

static void start_http(bool captive)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 6;
    /* render_form holds ~5.5 KB of locals at once (status, picker, two form
     * snprintf buffers, four escape buffers); plus snprintf/httpd internals
     * and IDF framework overhead the task wants ~14 KB to stay safe.
     * Anything tighter trips "stack overflow in task httpd" when a captive
     * portal request comes in. */
    cfg.stack_size = 16384;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = h_root };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    if (captive) {
        httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = h_catchall };
        httpd_register_uri_handler(s_httpd, &any);
    }
}

static void start_mdns(const char *device_id)
{
    char host[48];
    snprintf(host, sizeof host, "tesserae-%s", device_id);
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed; settings page only reachable by IP");
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set("Tesserae settings");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_up = true;
    ESP_LOGI(TAG, "mdns up: http://%s.local/", host);
}

static void stop_mdns(void)
{
    if (s_mdns_up) { mdns_free(); s_mdns_up = false; }
}

/* ---------- AP-idle tracker ---------- */

/* WiFi event handler for SoftAP STA join/leave. Each join bumps the client
 * counter (and disables the idle deadline while >=1 STA is associated);
 * each leave decrements it and, if no STAs are left, restarts the
 * PROVISION_PORTAL_TIMEOUT_S idle window. */
static void on_ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;

    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        s_idle_deadline_us = 0;   /* clients present -> no idle timeout */
        ESP_LOGI(TAG, "AP STA connected (clients=%d); idle timer paused",
                 s_ap_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        if (s_ap_clients == 0) {
            s_idle_deadline_us = esp_timer_get_time() +
                (int64_t)PROVISION_PORTAL_TIMEOUT_S * 1000000LL;
            ESP_LOGI(TAG, "AP STA disconnected (no clients); "
                          "idle countdown restarted (%ds)",
                     PROVISION_PORTAL_TIMEOUT_S);
        } else {
            ESP_LOGI(TAG, "AP STA disconnected (clients=%d)", s_ap_clients);
        }
    }
}

static void idle_tracker_install(void)
{
    s_ap_clients = 0;
    s_idle_deadline_us = esp_timer_get_time() +
        (int64_t)PROVISION_PORTAL_TIMEOUT_S * 1000000LL;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_ap_event, NULL, &s_ap_evt_handle));
}

static void idle_tracker_uninstall(void)
{
    if (s_ap_evt_handle) {
        esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, s_ap_evt_handle);
        s_ap_evt_handle = NULL;
    }
}

/* ---------- public API ---------- */

esp_err_t provisioning_run_blocking(void)
{
    s_done = xEventGroupCreate();

    /* Quick STA scan first so the form can offer a click-to-fill picker of
     * nearby networks; runs only on the captive-portal path (not the
     * always-on settings editor, where we're already STA-associated). */
    do_wifi_scan();

    start_ap();
    idle_tracker_install();
    start_http(/* captive */ true);
    xTaskCreate(dns_hijack_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "captive portal up; %ds idle timeout (resets on each STA join)",
             PROVISION_PORTAL_TIMEOUT_S);

    /* Poll in 1 s chunks so we can react to either a credential save OR the
     * idle deadline elapsing without a client connected. The idle deadline
     * is zeroed by the AP event handler whenever >=1 STA is associated. */
    EventBits_t bits = 0;
    while (1) {
        bits = xEventGroupWaitBits(s_done, BIT_CREDS_SAVED,
                                   pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & BIT_CREDS_SAVED) break;
        if (s_idle_deadline_us != 0 &&
            esp_timer_get_time() > s_idle_deadline_us) {
            ESP_LOGW(TAG, "captive portal idle for %ds with no client; giving up",
                     PROVISION_PORTAL_TIMEOUT_S);
            break;
        }
    }

    /* Give the browser a beat to render the "saved" page before we tear AP down. */
    vTaskDelay(pdMS_TO_TICKS(500));

    idle_tracker_uninstall();

    if (s_dns_task) { vTaskDelete(s_dns_task); s_dns_task = NULL; }
    if (s_httpd)    { httpd_stop(s_httpd);     s_httpd = NULL; }
    esp_wifi_stop();

    return (bits & BIT_CREDS_SAVED) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t settings_server_run_blocking(void)
{
    s_done = xEventGroupCreate();

    mqtt_config_t cfg;
    mqtt_config_load(&cfg);

    start_http(/* captive */ false);
    start_mdns(cfg.device_id);

    char ip[16] = {0};
    wifi_manager_get_sta_ip(ip, sizeof ip);
    ESP_LOGI(TAG, "settings server up at http://tesserae-%s.local/ (http://%s/); "
                  "up to %ds before sleep", cfg.device_id, ip[0] ? ip : "?",
             PROVISION_PORTAL_TIMEOUT_S);

    EventBits_t bits = xEventGroupWaitBits(
        s_done, BIT_CREDS_SAVED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(PROVISION_PORTAL_TIMEOUT_S * 1000));

    vTaskDelay(pdMS_TO_TICKS(500));   /* let the "saved" page flush */

    stop_mdns();
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }

    return (bits & BIT_CREDS_SAVED) ? ESP_OK : ESP_ERR_TIMEOUT;
}
