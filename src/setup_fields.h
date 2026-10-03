/*
 * The settings a person gives a panel so it can reach its dashboards, and the
 * rules they must satisfy, in one place for the three ways they arrive: the
 * captive portal's form, the USB serial setup (serial_setup.c) and, in
 * time, the BLE setup. Pure C, host-tested (test/test_setup_fields.c).
 *
 * Three ways to connect:
 *   local  the person's own Tesserae server: a URL, and a pairing code only
 *          when that server asks for one. The default.
 *   cloud  Tesserae Cloud: the URL is fixed and a claim code is always needed.
 *   relay  the cloud relay to a server elsewhere: a relay pairing code.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SETUP_CLOUD_URL "https://cloud.tesserae.ink"

typedef enum {
    SETUP_MODE_LOCAL = 0,
    SETUP_MODE_CLOUD,
    SETUP_MODE_RELAY,
} setup_mode_t;

typedef struct {
    char         ssid[33];
    char         pass[65];
    bool         have_pass;       /* false keeps the stored password */
    setup_mode_t mode;
    char         server_url[160]; /* local; pinned to SETUP_CLOUD_URL for cloud */
    char         pairing[16];     /* local (optional) or cloud (required) */
    char         relay_url[160];  /* relay; the default when empty */
    char         relay_code[24];  /* relay (required) */
} setup_fields_t;

/* "local", "cloud" or "relay"; an empty or unknown name is local, and the
 * function says whether the name was one it knows. */
bool        setup_mode_parse(const char *name, setup_mode_t *out);
const char *setup_mode_name(setup_mode_t mode);

/* Fills in what is implied (http:// on a bare host, the cloud URL, the default
 * relay) and checks what each mode needs. On a problem, writes a sentence for
 * the person into err and returns false. */
bool setup_fields_check(setup_fields_t *f, const char *relay_default, char *err, size_t err_cap);

/*
 * A flat JSON object, enough for the setup messages: {"key":"value",...} with
 * string, number, true/false/null values and the usual escapes. Nested values
 * are skipped. setup_json_get copies the value of key as text (numbers and
 * words as written, null as empty) and says whether the key was there.
 */
bool   setup_json_get(const char *json, const char *key, char *out, size_t cap);
/* Escapes s as a JSON string body (no quotes), returns the length written. */
size_t setup_json_escape(const char *s, char *out, size_t cap);
