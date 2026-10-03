#include "setup_fields.h"

#include <stdio.h>
#include <string.h>

bool setup_mode_parse(const char *name, setup_mode_t *out)
{
    if (!name || !name[0] || strcmp(name, "local") == 0) { *out = SETUP_MODE_LOCAL; return true; }
    if (strcmp(name, "cloud") == 0) { *out = SETUP_MODE_CLOUD; return true; }
    if (strcmp(name, "relay") == 0) { *out = SETUP_MODE_RELAY; return true; }
    *out = SETUP_MODE_LOCAL;
    return false;
}

const char *setup_mode_name(setup_mode_t mode)
{
    switch (mode) {
    case SETUP_MODE_CLOUD: return "cloud";
    case SETUP_MODE_RELAY: return "relay";
    default:               return "local";
    }
}

static bool has_scheme(const char *url)
{
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = '\0';
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, n - i + 1);
}

bool setup_fields_check(setup_fields_t *f, const char *relay_default, char *err, size_t err_cap)
{
    trim(f->ssid); trim(f->server_url); trim(f->pairing); trim(f->relay_url); trim(f->relay_code);
    if (!f->ssid[0]) { snprintf(err, err_cap, "WiFi network name (SSID) is required."); return false; }
    switch (f->mode) {
    case SETUP_MODE_CLOUD:
        /* The cloud is one place; whatever was typed for a server does not apply. */
        snprintf(f->server_url, sizeof f->server_url, "%s", SETUP_CLOUD_URL);
        if (!f->pairing[0]) { snprintf(err, err_cap, "A claim code from Tesserae Cloud is required. Make one under Settings &rarr; Panels."); return false; }
        f->relay_url[0] = f->relay_code[0] = '\0';
        break;
    case SETUP_MODE_RELAY:
        if (!f->relay_code[0]) { snprintf(err, err_cap, "A cloud-relay pairing code is required for a remote panel. Get one from Settings &rarr; Cloud relay &rarr; Add a remote panel."); return false; }
        if (!f->relay_url[0]) snprintf(f->relay_url, sizeof f->relay_url, "%s", relay_default);
        /* The relay is public and always TLS; anything else would ship the pairing handshake in the clear. */
        if (strncmp(f->relay_url, "https://", 8) != 0) { snprintf(err, err_cap, "The relay URL must start with https://."); return false; }
        f->server_url[0] = f->pairing[0] = '\0';
        break;
    default:
        if (!f->server_url[0]) { snprintf(err, err_cap, "Tesserae server URL is required."); return false; }
        /* Be forgiving about the scheme: a bare host means http://. */
        if (!has_scheme(f->server_url)) {
            char with_scheme[sizeof f->server_url + 8];
            snprintf(with_scheme, sizeof with_scheme, "http://%s", f->server_url);
            snprintf(f->server_url, sizeof f->server_url, "%s", with_scheme);
        }
        f->relay_url[0] = f->relay_code[0] = '\0';
        break;
    }
    /* A trailing slash on an origin doubles up in every path the firmware builds. */
    size_t n = strlen(f->server_url);
    while (n > 8 && f->server_url[n - 1] == '/') f->server_url[--n] = '\0';
    n = strlen(f->relay_url);
    while (n > 8 && f->relay_url[n - 1] == '/') f->relay_url[--n] = '\0';
    return true;
}

/* ---- flat JSON ---- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put(char *out, size_t cap, size_t *n, char c)
{
    if (out && *n + 1 < cap) out[*n] = c;
    (*n)++;
}

/* Reads a JSON string starting at the opening quote; writes the decoded text
 * (NUL terminated, truncated to cap) and returns the position after the
 * closing quote, or NULL when the string never closes. */
static const char *read_string(const char *p, char *out, size_t cap)
{
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                int v = 0;
                for (int i = 0; i < 4; i++) { int h = hexval(*p); if (h < 0) return NULL; v = v * 16 + h; p++; }
                /* Basic Multilingual Plane as UTF-8; a surrogate pair is two escapes and is kept as '?'. */
                if (v < 0x80) { c = (char)v; }
                else if (v < 0x800) { put(out, cap, &n, (char)(0xC0 | (v >> 6))); c = (char)(0x80 | (v & 0x3F)); }
                else if (v >= 0xD800 && v <= 0xDFFF) { c = '?'; }
                else { put(out, cap, &n, (char)(0xE0 | (v >> 12))); put(out, cap, &n, (char)(0x80 | ((v >> 6) & 0x3F))); c = (char)(0x80 | (v & 0x3F)); }
                break;
            }
            case '\0': return NULL;
            default: c = e; break;
            }
        }
        put(out, cap, &n, c);
    }
    if (*p != '"') return NULL;
    if (out && cap) out[n < cap ? n : cap - 1] = '\0';
    return p + 1;
}

/* Skips any value; returns the position after it, or NULL when malformed. */
static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') return read_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        char open = *p, close = open == '{' ? '}' : ']';
        int depth = 0;
        for (;;) {
            if (!*p) return NULL;
            if (*p == '"') { p = read_string(p, NULL, 0); if (!p) return NULL; continue; }
            if (*p == open) depth++;
            else if (*p == close && --depth == 0) return p + 1;
            p++;
        }
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
    return p;
}

bool setup_json_get(const char *json, const char *key, char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    const char *p = skip_ws(json);
    if (*p != '{') return false;
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}' || !*p) return false;
        char k[48];
        p = read_string(p, k, sizeof k);
        if (!p) return false;
        p = skip_ws(p);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
        if (strcmp(k, key) == 0) {
            if (*p == '"') return read_string(p, out, cap) != NULL;
            if (*p == '{' || *p == '[') return false;   /* nested values are not setup values */
            const char *end = skip_value(p);
            if (!end) return false;
            size_t len = (size_t)(end - p);
            if (len == 4 && strncmp(p, "null", 4) == 0) len = 0;
            if (out && cap) { if (len >= cap) len = cap - 1; memcpy(out, p, len); out[len] = '\0'; }
            return true;
        }
        p = skip_value(p);
        if (!p) return false;
        p = skip_ws(p);
        if (*p == ',') p++;
    }
}

size_t setup_json_escape(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *rep = NULL;
        char buf[7];
        switch (c) {
        case '"': rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default:
            if (c < 0x20) { snprintf(buf, sizeof buf, "\\u%04x", c); rep = buf; }
            break;
        }
        if (rep) { for (const char *r = rep; *r; r++) put(out, cap, &n, *r); }
        else put(out, cap, &n, (char)c);
    }
    if (out && cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}
