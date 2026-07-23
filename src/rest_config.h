/*
 * rest_config.h: NVS-backed configuration for the Tesserae REST transport.
 *
 * Replaces the MQTT config (mqtt_config.[ch]). Holds the server origin, the
 * device bearer token, an optional one-shot pairing code, the canonical device
 * id (adopted from the server, which matches devices by MAC), the cached frame
 * ETag (for If-None-Match dedup across wakes), and the deep-sleep interval.
 *
 * Modelled on tesserae-device-pico-bin's config_t: the whole config is loaded
 * into a RAM cache at boot; mutators update the cache; rest_config_save()
 * persists it. Stored under NVS namespace "rest"; WiFi creds stay in "wifi".
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app_config.h"   /* BOARD_HAS_TOUCH gates the touch config below */
#include "deck.h"         /* DECK_ID_CAP / DECK_VERSION_CAP for nav state */

typedef struct {
    char    server_url[160];      /* e.g. http://tesserae.local:8765 (no trailing slash) */
    char    device_token[256];    /* bearer token, set after discover/register */
    char    pairing_code[16];     /* set pre-register, cleared on success */
    char    device_id[33];        /* canonical id (server-assigned, MAC-matched) */
    char    last_frame_etag[80];  /* cached across wakes for If-None-Match */
    int32_t sleep_s;              /* deep-sleep interval, seconds */
    int32_t button_wake_s;        /* server config: stay awake N s after a button
                                     press listening for more (0-60, 0 = off) */
#if BOARD_HAS_TOUCH
    bool    touch_enabled;        /* server config: arm GT911 touch wake (default false) */
    int32_t touch_linger_s;       /* server config: stay awake N s after a touch (0-60) */
#endif
    /* Deck cache nav state (SD-card decks; deck.h). The RTC copy in main.c is
     * authoritative across deep sleeps; this NVS mirror survives RESET. */
    char    deck_id[DECK_ID_CAP];
    char    deck_page[DECK_ID_CAP];            /* current page id */
    char    deck_synced_ver[DECK_VERSION_CAP]; /* manifest version fully on card */
    char    deck_srv_ver[DECK_VERSION_CAP];    /* last server-announced version */
    bool    deck_sd_painted;      /* displayed frame came from the SD cache */
} rest_config_t;

/* Load config from NVS into the RAM cache. Never fails; missing keys default
 * (sleep_s -> SLEEP_INTERVAL_S, others empty / compile-time defaults). */
void rest_config_load(void);

/* The in-RAM cache; never NULL (call rest_config_load() first). */
const rest_config_t *rest_config_get(void);

/* Persist the RAM cache to NVS. Call only when something changed (flash wear).
 * Radio may be up (NVS on ESP32 is safe with WiFi, unlike the pico's flash). */
esp_err_t rest_config_save(void);

/* Effective device id: the cached device_id if set (server-assigned), else a
 * stable default "<TESSERAE_DEVICE_MODEL>_<mac-suffix>" (e.g.
 * reTerminal_E1004_859878). Cached; never NULL. */
const char *rest_config_device_id(void);

/* The base MAC as "aa:bb:cc:dd:ee:ff" (lowercase). The server matches the
 * device by this, independent of device_id. */
void rest_config_mac(char *out, size_t cap);

/* True if a REST server URL is configured (i.e. the device is provisioned). */
bool rest_config_has_server(void);

/* Mutators: update the RAM cache. NULL leaves a field unchanged; "" clears it.
 * Call rest_config_save() to persist. */
void rest_config_set_server(const char *url);
void rest_config_set_pairing(const char *code);
void rest_config_set_device_id(const char *id);
void rest_config_set_device_token(const char *token);
void rest_config_set_frame_etag(const char *etag);
void rest_config_set_sleep_s(int32_t s);

/* Post-button stay-awake window (issue #123), delivered in the /frame 200 body
 * and the register/status "config" objects. Clamped to 0-60 s. Persist with
 * rest_config_save. */
void rest_config_set_button_wake_s(int32_t s);

#if BOARD_HAS_TOUCH
/* Touch config, delivered in the status response's "config" object (like
 * sleep_interval_s). linger is clamped to 0-60 s. Persist with rest_config_save. */
void rest_config_set_touch(bool enabled, int32_t linger_s);
#endif

/* Deck nav state mutators (NULL leaves a field unchanged; "" clears). RAM
 * cache updates like every other mutator -- persist with rest_config_save(). */
void rest_config_set_deck_nav(const char *deck_id, const char *page_id);
void rest_config_set_deck_synced_ver(const char *version);
void rest_config_set_deck_srv_ver(const char *version);
void rest_config_set_deck_sd_painted(bool painted);

/* Persisted onboarding-splash state (0 = none/fresh, board-defined otherwise),
 * used to repaint connect-status splashes only when the state changes. */
uint8_t rest_config_get_ui_state(void);
void    rest_config_set_ui_state(uint8_t v);
