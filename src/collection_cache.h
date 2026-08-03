/*
 * collection_cache.h: SD-card file layer for producer-neutral frame caches.
 *
 * Collection ids are server-owned strings and may contain FAT-illegal path
 * characters (the first Album ids use "album:<slug>"). The on-card directory
 * is therefore a stable 16-hex SHA-256 of the id; the original id remains in
 * manifest.json and is verified after parsing.
 *
 *   /sdcard/tesserae/collections/c_<id-sha16>/manifest.json
 *   /sdcard/tesserae/collections/c_<id-sha16>/<frame-sha16>.bin
 *
 * The card is disposable and untrusted. Every frame read and write verifies
 * exact length plus digest, and failed reads delete the corrupt cache entry.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "frame_collection.h"

bool collection_cache_load_manifest(const char *collection_id,
                                    fc_manifest_t *out);
bool collection_cache_save_manifest(const char *collection_id,
                                    const char *json, size_t len);

bool collection_cache_read_frame(const char *collection_id, const char *digest,
                                 uint32_t expect_bytes, uint8_t **out);
bool collection_cache_write_frame(const char *collection_id, const char *digest,
                                  const uint8_t *data, size_t len,
                                  uint32_t expect_bytes);

int collection_cache_list(const char *collection_id,
                          char digests[][FC_DIGEST_HEX + 1], int max);
void collection_cache_delete(const char *collection_id, const char *digest);
int32_t collection_cache_frame_age_s(const char *collection_id,
                                     const char *digest);
