/* relay_wire.c -- pure cloud-relay wire handling. See relay_wire.h. */

#include "relay_wire.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static bool copy_field(char *dst, size_t cap, const cJSON *v)
{
    if (!cJSON_IsString(v) || !v->valuestring[0]) return false;
    size_t n = strlen(v->valuestring);
    if (n >= cap) return false;          /* truncation would corrupt an id */
    memcpy(dst, v->valuestring, n + 1);
    return true;
}

relay_ready_t relay_parse_ready(const char *json, size_t len,
                                relay_pairing_t *out)
{
    if (!json || !out) return RELAY_READY_MALFORMED;
    memset(out, 0, sizeof *out);

    cJSON *r = cJSON_ParseWithLength(json, len);
    if (!r) return RELAY_READY_MALFORMED;
    relay_ready_t rc = RELAY_READY_MALFORMED;

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
    if (!cJSON_IsString(status)) goto done;
    if (strcmp(status->valuestring, "ready") != 0) {
        /* Anything that is not "ready" is simply not done yet. */
        rc = RELAY_READY_PENDING;
        goto done;
    }

    const cJSON *config = cJSON_GetObjectItemCaseSensitive(r, "config");
    const cJSON *inst   = cJSON_GetObjectItemCaseSensitive(r, "install_id");
    if (!cJSON_IsString(inst) && cJSON_IsObject(config))
        inst = cJSON_GetObjectItemCaseSensitive(config, "install_id");

    if (!copy_field(out->install_id, sizeof out->install_id, inst) ||
        !copy_field(out->device_id, sizeof out->device_id,
                    cJSON_GetObjectItemCaseSensitive(r, "device_id")) ||
        !copy_field(out->device_token, sizeof out->device_token,
                    cJSON_GetObjectItemCaseSensitive(r, "device_token")))
        goto done;

    const cJSON *hp = cJSON_GetObjectItemCaseSensitive(r, "home_pubkey");
    if (!cJSON_IsString(hp)) goto done;
    size_t n = 0;
    if (!relay_b64url_decode(out->home_pub, sizeof out->home_pub, &n,
                             hp->valuestring) || n != RELAY_PUB_LEN)
        goto done;

    rc = RELAY_READY_OK;

done:
    cJSON_Delete(r);
    if (rc != RELAY_READY_OK) memset(out, 0, sizeof *out);
    return rc;
}

bool relay_build_pair_body(char *out, size_t cap,
                           const char *code, const char *pubkey_b64,
                           int panel_w, int panel_h,
                           const char *model, const char *gamut)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!code || !code[0] || !pubkey_b64 || !pubkey_b64[0]) return false;

    cJSON *o = cJSON_CreateObject();
    if (!o) return false;
    bool ok = cJSON_AddStringToObject(o, "code", code) &&
              cJSON_AddStringToObject(o, "panel_pubkey", pubkey_b64);
    /* Optional self-report; each field is independent, so a board that knows
     * its geometry but not its gamut still reports the geometry. */
    if (ok && panel_w > 0) ok = cJSON_AddNumberToObject(o, "panel_w", panel_w);
    if (ok && panel_h > 0) ok = cJSON_AddNumberToObject(o, "panel_h", panel_h);
    if (ok && model && model[0]) ok = cJSON_AddStringToObject(o, "model", model);
    if (ok && gamut && gamut[0]) ok = cJSON_AddStringToObject(o, "gamut", gamut);

    char *body = ok ? cJSON_PrintUnformatted(o) : NULL;
    cJSON_Delete(o);
    if (!body) return false;

    size_t n = strlen(body);
    if (n >= cap) { cJSON_free(body); return false; }
    memcpy(out, body, n + 1);
    cJSON_free(body);
    return true;
}

bool relay_mailbox_url(char *out, size_t cap, const char *base,
                       const char *install_id, const char *device_id,
                       const char *leaf)
{
    if (!out || !cap) return false;
    out[0] = '\0';
    if (!base || !base[0] || !install_id || !install_id[0] ||
        !device_id || !device_id[0] || !leaf || !leaf[0]) return false;

    int n = snprintf(out, cap, "%s/v1/i/%s/d/%s/%s",
                     base, install_id, device_id, leaf);
    if (n < 0 || (size_t)n >= cap) {     /* truncated -> refuse, never request */
        out[0] = '\0';
        return false;
    }
    return true;
}
