/*
 * deck.h: pure deck-cache logic -- manifest parsing, link hit-testing, digest
 * rules, and the cache sync differ. NO ESP-IDF dependencies: this compiles on
 * the host for unit tests (test/test_deck.c) exactly like button_report.c.
 *
 * A "deck" is a small graph of pre-rendered pages the user navigates with
 * buttons or touch. The server serves a manifest (GET /api/v1/device/<id>/deck)
 * describing pages, their frame digests, and navigation links; the frames are
 * cached on SD card so a nav press paints without the radio. See deck_cache.h
 * for the device-side file layer and main.c for the wake-loop integration.
 *
 * Digests are the first 16 chars of lowercase-hex sha256(frame bytes). The
 * SHA-256 backend is intentionally NOT here: the device links deck_digest.c
 * (mbedTLS; Monocypher has no SHA-256), the host test links its own reference
 * implementation. Both feed deck_digest_check() below.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Caps chosen for "small graph of pages": a manifest that exceeds them fails
 * the parse, and the device falls back to plain network fetches (the cache is
 * an optimisation, never a requirement). */
#define DECK_MAX_PAGES     32
#define DECK_MAX_LINKS      8
#define DECK_ID_CAP        48   /* deck_id / page_id strings, NUL included */
#define DECK_VERSION_CAP   32
#define DECK_DIGEST_HEX    16   /* first 16 hex chars of sha256 */
#define DECK_BUTTON_CAP     8   /* "refresh" / "left" / "right" */

typedef struct {
    char  button[DECK_BUTTON_CAP];   /* "" when this is a zone link */
    bool  has_zone;
    float zx, zy, zw, zh;            /* normalised 0..1 rect */
    char  target_page_id[DECK_ID_CAP];
} deck_link_t;

typedef struct {
    char        page_id[DECK_ID_CAP];
    char        digest[DECK_DIGEST_HEX + 1];
    uint32_t    bytes;
    int32_t     ttl_s;
    int         n_links;
    deck_link_t links[DECK_MAX_LINKS];
} deck_page_t;

typedef struct {
    char        deck_id[DECK_ID_CAP];
    char        version[DECK_VERSION_CAP];
    char        entry_page_id[DECK_ID_CAP];
    int         n_pages;
    deck_page_t pages[DECK_MAX_PAGES];
} deck_manifest_t;

/* Parse the GET /deck 200 body. Strict: missing/malformed required fields,
 * a digest that is not exactly 16 lowercase hex chars, bytes == 0, or more
 * than DECK_MAX_PAGES pages all fail the parse (return false, *out zeroed).
 * Links with neither button nor zone, or with an unknown target page, are
 * DROPPED individually (a bad link should not take the whole deck down). */
bool deck_manifest_parse(const char *json, size_t len, deck_manifest_t *out);

/* Find a page by id; NULL if absent. */
const deck_page_t *deck_find_page(const deck_manifest_t *m, const char *page_id);

/* Button hit-test: the target page id for `button` ("refresh"/"left"/"right")
 * on `page_id`, or NULL when no link matches. First matching link wins. */
const char *deck_nav_button(const deck_manifest_t *m, const char *page_id,
                            const char *button);

/* Touch hit-test: (x, y) in panel pixel space, panel w x h. Zones are
 * normalised 0..1 rects; the PIXEL CENTRE decides, i.e. pixel x hits when
 * zx <= (x + 0.5)/w < zx + zw (left/top edge inclusive, right/bottom edge
 * exclusive), so abutting zones never both claim a boundary pixel. First
 * matching zone link wins. NULL when nothing matches. */
const char *deck_nav_touch(const deck_manifest_t *m, const char *page_id,
                           int x, int y, int panel_w, int panel_h);

/* Cache sync differ. `have` is the list of digests currently on card for this
 * deck. Emits up to max_fetch digests that the manifest references but the
 * card lacks (deduplicated) into `fetch`, and up to max_orphan digests on card
 * that no page references into `orphan`. Returns the fetch count; *n_orphan
 * (optional) receives the orphan count. Overflowing either cap just truncates
 * that list -- the next wake's sync picks up the remainder. */
int deck_sync_plan(const deck_manifest_t *m,
                   const char have[][DECK_DIGEST_HEX + 1], int n_have,
                   char fetch[][DECK_DIGEST_HEX + 1], int max_fetch,
                   char orphan[][DECK_DIGEST_HEX + 1], int max_orphan,
                   int *n_orphan);

/* ---- digest rules ---- */

/* SHA-256 backend, supplied by the build: deck_digest.c (mbedTLS) on device,
 * the test's reference implementation on host. */
void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Lowercase-hex encode the first 8 bytes of a sha256 into out[17]. */
void deck_digest_hex16(const uint8_t sha[32], char out[DECK_DIGEST_HEX + 1]);

/* Verify a frame blob against the manifest's contract: exact byte length AND
 * digest match. Rejects on any mismatch. `expect_digest` must be the 16-char
 * lowercase-hex form (as parsed). */
bool deck_digest_check(const uint8_t *frame, size_t len,
                       uint32_t expect_bytes, const char *expect_digest);

/* True iff s is exactly 16 lowercase hex chars. */
bool deck_digest_valid(const char *s);
