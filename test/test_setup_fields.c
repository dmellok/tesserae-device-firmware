#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "setup_fields.h"

static void test_json(void)
{
    char v[64];
    const char *msg = "{\"tesserae\":\"setup\", \"cmd\":\"set\",\"ssid\":\"caf\\u00e9 \\\"net\\\"\",\"n\":42,\"ok\":true,\"none\":null,\"obj\":{\"a\":[1,2,\"}\"]},\"last\":\"x\"}";
    assert(setup_json_get(msg, "tesserae", v, sizeof v) && strcmp(v, "setup") == 0);
    assert(setup_json_get(msg, "ssid", v, sizeof v) && strcmp(v, "caf\xc3\xa9 \"net\"") == 0);
    assert(setup_json_get(msg, "n", v, sizeof v) && strcmp(v, "42") == 0);
    assert(setup_json_get(msg, "ok", v, sizeof v) && strcmp(v, "true") == 0);
    assert(setup_json_get(msg, "none", v, sizeof v) && v[0] == '\0');
    assert(!setup_json_get(msg, "obj", v, sizeof v));
    assert(setup_json_get(msg, "last", v, sizeof v) && strcmp(v, "x") == 0);
    assert(!setup_json_get(msg, "missing", v, sizeof v));
    assert(!setup_json_get("not json", "a", v, sizeof v));
    assert(!setup_json_get("{\"a\":\"unterminated", "a", v, sizeof v));
    /* Truncation keeps the terminator. */
    char small[4];
    assert(setup_json_get("{\"k\":\"abcdefgh\"}", "k", small, sizeof small) && strcmp(small, "abc") == 0);
    char esc[32];
    setup_json_escape("a\"b\\c\nd", esc, sizeof esc);
    assert(strcmp(esc, "a\\\"b\\\\c\\nd") == 0);
}

static void test_modes(void)
{
    setup_mode_t m;
    assert(setup_mode_parse("", &m) && m == SETUP_MODE_LOCAL);
    assert(setup_mode_parse("cloud", &m) && m == SETUP_MODE_CLOUD);
    assert(setup_mode_parse("relay", &m) && m == SETUP_MODE_RELAY);
    assert(!setup_mode_parse("mars", &m) && m == SETUP_MODE_LOCAL);
    assert(strcmp(setup_mode_name(SETUP_MODE_CLOUD), "cloud") == 0);
}

static void test_check(void)
{
    char err[160];
    setup_fields_t f;
    memset(&f, 0, sizeof f);
    assert(!setup_fields_check(&f, "https://relay.example", err, sizeof err) && strstr(err, "SSID"));

    /* Local: the scheme is implied and a trailing slash dropped. */
    memset(&f, 0, sizeof f);
    strcpy(f.ssid, " home ");
    strcpy(f.server_url, "tesserae.local:8765/");
    assert(setup_fields_check(&f, "https://relay.example", err, sizeof err));
    assert(strcmp(f.ssid, "home") == 0);
    assert(strcmp(f.server_url, "http://tesserae.local:8765") == 0);
    memset(&f, 0, sizeof f);
    strcpy(f.ssid, "home");
    assert(!setup_fields_check(&f, "https://relay.example", err, sizeof err) && strstr(err, "server URL"));

    /* Cloud: the URL is pinned whatever was typed, and a code is required. */
    memset(&f, 0, sizeof f);
    strcpy(f.ssid, "home"); f.mode = SETUP_MODE_CLOUD; strcpy(f.server_url, "http://elsewhere");
    assert(!setup_fields_check(&f, "https://relay.example", err, sizeof err) && strstr(err, "claim code"));
    strcpy(f.pairing, "12345678"); strcpy(f.relay_code, "stale");
    assert(setup_fields_check(&f, "https://relay.example", err, sizeof err));
    assert(strcmp(f.server_url, SETUP_CLOUD_URL) == 0 && f.relay_code[0] == '\0');

    /* Relay: a code, the default relay, and https only. */
    memset(&f, 0, sizeof f);
    strcpy(f.ssid, "home"); f.mode = SETUP_MODE_RELAY;
    assert(!setup_fields_check(&f, "https://relay.example", err, sizeof err) && strstr(err, "relay pairing code"));
    strcpy(f.relay_code, "ABCD-EFGH"); strcpy(f.server_url, "http://home");
    assert(setup_fields_check(&f, "https://relay.example", err, sizeof err));
    assert(strcmp(f.relay_url, "https://relay.example") == 0 && f.server_url[0] == '\0');
    strcpy(f.relay_url, "http://plain.example");
    assert(!setup_fields_check(&f, "https://relay.example", err, sizeof err) && strstr(err, "https://"));
}

int main(void)
{
    test_json();
    test_modes();
    test_check();
    printf("setup_fields: ok\n");
    return 0;
}
