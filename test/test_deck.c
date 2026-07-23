/*
 * Host-side unit tests for deck.c: manifest parse, link hit-testing (button
 * and zone incl. edge coordinates), digest verify accept/reject, and the
 * cache sync differ. Build + run (see tools/test_deck.sh):
 *
 *   cc -std=c11 -Wall -Wextra -Werror -I src -I <cJSON dir> \
 *      test/test_deck.c src/deck.c <cJSON dir>/cJSON.c -o /tmp/td && /tmp/td
 *
 * Supplies the deck_sha256() backend with a self-contained reference SHA-256
 * (public-domain style, validated against FIPS 180-2 vectors in the tests);
 * the device build links src/deck_digest.c (mbedTLS) instead.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "deck.h"

/* ---------- reference SHA-256 (host test backend) ---------- */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t fill; } sha_t;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(sha_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4]<<24 | (uint32_t)p[i*4+1]<<16 |
               (uint32_t)p[i*4+2]<<8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4],f=s->h[5],g=s->h[6],h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

void deck_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    sha_t s = {.h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19},
               .len = (uint64_t)len * 8, .fill = 0};
    while (len >= 64) { sha_block(&s, data); data += 64; len -= 64; }
    memcpy(s.buf, data, len);
    s.fill = len;
    s.buf[s.fill++] = 0x80;
    if (s.fill > 56) { memset(s.buf + s.fill, 0, 64 - s.fill); sha_block(&s, s.buf); s.fill = 0; }
    memset(s.buf + s.fill, 0, 56 - s.fill);
    for (int i = 0; i < 8; i++) s.buf[56 + i] = (uint8_t)(s.len >> (56 - 8 * i));
    sha_block(&s, s.buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s.h[i] >> 24);
        out[i*4+1] = (uint8_t)(s.h[i] >> 16);
        out[i*4+2] = (uint8_t)(s.h[i] >> 8);
        out[i*4+3] = (uint8_t)(s.h[i]);
    }
}

/* ---------- fixtures ---------- */

static const char GOOD[] =
"{"
"  \"deck_id\": \"deck-1\", \"version\": \"v7\", \"entry_page_id\": \"home\","
"  \"pages\": ["
"    {\"page_id\": \"home\", \"digest\": \"00112233445566aa\", \"bytes\": 48000,"
"     \"ttl_s\": 3600,"
"     \"links\": ["
"       {\"button\": \"right\", \"zone\": null, \"target_page_id\": \"stats\"},"
"       {\"button\": null, \"zone\": {\"x\": 0.5, \"y\": 0.0, \"w\": 0.5, \"h\": 1.0},"
"        \"target_page_id\": \"stats\"},"
"       {\"button\": \"left\", \"zone\": null, \"target_page_id\": \"gone\"}"
"     ]},"
"    {\"page_id\": \"stats\", \"digest\": \"ffeeddccbbaa9988\", \"bytes\": 48000,"
"     \"ttl_s\": 60,"
"     \"links\": [{\"button\": \"left\", \"zone\": null, \"target_page_id\": \"home\"}]}"
"  ]"
"}";

static int tests = 0, fails = 0;
#define CHECK(cond) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    /* ---- manifest parse ---- */
    deck_manifest_t m;
    CHECK(deck_manifest_parse(GOOD, sizeof GOOD - 1, &m));
    CHECK(strcmp(m.deck_id, "deck-1") == 0);
    CHECK(strcmp(m.version, "v7") == 0);
    CHECK(m.n_pages == 2);
    CHECK(m.pages[0].bytes == 48000 && m.pages[0].ttl_s == 3600);
    /* the "gone" link targets a nonexistent page -> dropped; zone+button remain */
    CHECK(m.pages[0].n_links == 2);

    /* digest must be exactly 16 lowercase hex */
    {
        char bad[sizeof GOOD];
        memcpy(bad, GOOD, sizeof GOOD);
        char *d = strstr(bad, "00112233445566aa");
        d[0] = 'A';   /* uppercase -> invalid */
        deck_manifest_t t;
        CHECK(!deck_manifest_parse(bad, sizeof bad - 1, &t));
        d[0] = 'z';   /* non-hex -> invalid */
        CHECK(!deck_manifest_parse(bad, sizeof bad - 1, &t));
    }
    /* entry page must exist */
    {
        char bad[sizeof GOOD];
        memcpy(bad, GOOD, sizeof GOOD);
        char *e = strstr(bad, "\"home\"");   /* entry_page_id value */
        memcpy(e, "\"nope\"", 6);
        deck_manifest_t t;
        CHECK(!deck_manifest_parse(bad, sizeof bad - 1, &t));
    }
    /* bytes == 0 rejected */
    {
        char bad[sizeof GOOD + 8];
        memcpy(bad, GOOD, sizeof GOOD);
        char *b = strstr(bad, "48000");
        memcpy(b, "0    ", 5);
        deck_manifest_t t;
        CHECK(!deck_manifest_parse(bad, strlen(bad), &t));
    }
    /* not JSON */
    {
        deck_manifest_t t;
        CHECK(!deck_manifest_parse("hello", 5, &t));
        CHECK(!deck_manifest_parse(NULL, 0, &t));
    }

    /* ---- button hit-testing ---- */
    CHECK(deck_nav_button(&m, "home", "right") &&
          strcmp(deck_nav_button(&m, "home", "right"), "stats") == 0);
    CHECK(deck_nav_button(&m, "home", "left") == NULL);   /* dropped dead link */
    CHECK(deck_nav_button(&m, "stats", "left") &&
          strcmp(deck_nav_button(&m, "stats", "left"), "home") == 0);
    CHECK(deck_nav_button(&m, "stats", "right") == NULL);
    CHECK(deck_nav_button(&m, "nope", "right") == NULL);
    CHECK(deck_nav_button(&m, "home", "") == NULL);
    CHECK(deck_nav_button(&m, "home", NULL) == NULL);

    /* ---- zone hit-testing, 800x480 panel, zone x:[0.5,1.0) ---- */
    /* pixel 400 centre = 400.5/800 = 0.5006 -> inside (left edge inclusive) */
    CHECK(deck_nav_touch(&m, "home", 400, 240, 800, 480) != NULL);
    /* pixel 399 centre = 0.4994 -> outside */
    CHECK(deck_nav_touch(&m, "home", 399, 240, 800, 480) == NULL);
    /* rightmost pixel 799 centre = 0.99938 < 1.0 -> inside */
    CHECK(deck_nav_touch(&m, "home", 799, 0, 800, 480) != NULL);
    /* top and bottom edges of a full-height zone */
    CHECK(deck_nav_touch(&m, "home", 400, 0, 800, 480) != NULL);
    CHECK(deck_nav_touch(&m, "home", 400, 479, 800, 480) != NULL);
    /* out-of-panel coords never hit */
    CHECK(deck_nav_touch(&m, "home", -1, 0, 800, 480) == NULL);
    CHECK(deck_nav_touch(&m, "home", 800, 0, 800, 480) == NULL);
    CHECK(deck_nav_touch(&m, "home", 0, 480, 800, 480) == NULL);
    /* page with no zones */
    CHECK(deck_nav_touch(&m, "stats", 700, 240, 800, 480) == NULL);

    /* ---- digest verify ---- */
    {
        /* FIPS 180-2 vector: sha256("abc") =
         * ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad */
        uint8_t sha[32];
        deck_sha256((const uint8_t *)"abc", 3, sha);
        char hex[17];
        deck_digest_hex16(sha, hex);
        CHECK(strcmp(hex, "ba7816bf8f01cfea") == 0);

        CHECK(deck_digest_check((const uint8_t *)"abc", 3, 3, "ba7816bf8f01cfea"));
        /* wrong digest */
        CHECK(!deck_digest_check((const uint8_t *)"abc", 3, 3, "ba7816bf8f01cfeb"));
        /* wrong length vs manifest bytes */
        CHECK(!deck_digest_check((const uint8_t *)"abc", 3, 4, "ba7816bf8f01cfea"));
        /* malformed digest string */
        CHECK(!deck_digest_check((const uint8_t *)"abc", 3, 3, "BA7816BF8F01CFEA"));
        CHECK(!deck_digest_check((const uint8_t *)"abc", 3, 3, "ba7816bf8f01cfe"));
        /* empty frame */
        CHECK(!deck_digest_check(NULL, 0, 0, "ba7816bf8f01cfea"));
        /* multi-block message (> 64 bytes) exercises the block loop */
        static const char msg[] =
            "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        deck_sha256((const uint8_t *)msg, sizeof msg - 1, sha);
        deck_digest_hex16(sha, hex);
        CHECK(strcmp(hex, "248d6a61d20638b8") == 0);   /* FIPS vector 2 */
    }

    /* ---- sync differ ---- */
    {
        char have[4][DECK_DIGEST_HEX + 1];
        char fetch[8][DECK_DIGEST_HEX + 1];
        char orphan[8][DECK_DIGEST_HEX + 1];
        int no = -1;

        /* empty card: fetch both pages, no orphans */
        int nf = deck_sync_plan(&m, have, 0, fetch, 8, orphan, 8, &no);
        CHECK(nf == 2 && no == 0);

        /* one present, one stale file: fetch the other, delete the orphan */
        strcpy(have[0], "00112233445566aa");
        strcpy(have[1], "1111111111111111");
        nf = deck_sync_plan(&m, have, 2, fetch, 8, orphan, 8, &no);
        CHECK(nf == 1 && strcmp(fetch[0], "ffeeddccbbaa9988") == 0);
        CHECK(no == 1 && strcmp(orphan[0], "1111111111111111") == 0);

        /* fully synced: nothing to do */
        strcpy(have[1], "ffeeddccbbaa9988");
        nf = deck_sync_plan(&m, have, 2, fetch, 8, orphan, 8, &no);
        CHECK(nf == 0 && no == 0);

        /* duplicate digests across pages fetch once */
        deck_manifest_t dup = m;
        strcpy(dup.pages[1].digest, dup.pages[0].digest);
        nf = deck_sync_plan(&dup, have, 0, fetch, 8, orphan, 8, &no);
        CHECK(nf == 1);

        /* fetch-cap truncation is not an error */
        nf = deck_sync_plan(&m, have, 0, fetch, 1, orphan, 8, &no);
        CHECK(nf == 1);
    }

    printf("%d tests, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
