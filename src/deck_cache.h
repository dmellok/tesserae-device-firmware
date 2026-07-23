/*
 * deck_cache.h: SD-card file layer for the deck frame cache.
 *
 * Card layout (FATFS, 8.3-safe names -- digests are 16 hex chars):
 *   /sdcard/tesserae/decks/<deck_id>/manifest.json
 *   /sdcard/tesserae/decks/<deck_id>/<digest>.bin
 *
 * The cache is DISPOSABLE and the card is user-writable and untrusted: every
 * read is verified (exact byte length AND 16-hex sha256 digest) before use,
 * any failure just reports "not cached" and the caller falls back to the
 * network. Nothing the user put on the card outside /tesserae is ever
 * touched. All calls require sdcard_mounted(); they fail soft otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "deck.h"

/* Read + parse the cached manifest for deck_id. False when absent/corrupt. */
bool deck_cache_load_manifest(const char *deck_id, deck_manifest_t *out);

/* Persist the raw manifest JSON (already validated by deck_manifest_parse). */
bool deck_cache_save_manifest(const char *deck_id, const char *json, size_t len);

/* Read + VERIFY a cached frame. On success *out is a heap buffer of exactly
 * expect_bytes (caller free()s). Verification failure deletes the bad file. */
bool deck_cache_read_frame(const char *deck_id, const char *digest,
                           uint32_t expect_bytes, uint8_t **out);

/* Verify + write a fetched frame (tmp file + rename, so a power cut never
 * leaves a plausible-looking partial). Rejects digest/length mismatches. */
bool deck_cache_write_frame(const char *deck_id, const char *digest,
                            const uint8_t *data, size_t len, uint32_t expect_bytes);

/* List the <digest>.bin files present for deck_id. Returns the count (<= max). */
int deck_cache_list(const char *deck_id,
                    char digests[][DECK_DIGEST_HEX + 1], int max);

/* Delete one cached frame (orphan cleanup). */
void deck_cache_delete(const char *deck_id, const char *digest);

/* Age of a cached frame in seconds (file mtime vs the RTC wall clock), for
 * the manifest ttl_s rule. Returns INT32_MAX when the file is missing or
 * either clock is implausible -- callers treat that as expired. */
int32_t deck_cache_frame_age_s(const char *deck_id, const char *digest);
