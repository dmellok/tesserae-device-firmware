/*
 * touch_queue.h -- RTC-backed queue of captured touch strokes.
 *
 * A touch captured on a wake is normally dispatched on that wake's frame GET.
 * If WiFi fails to connect that wake, the stroke would be lost. This small ring
 * buffer lives in RTC memory (survives deep sleep), so a failed-to-send touch is
 * replayed on the next successful connect. Only actually-captured strokes are
 * queued (a coordinate-less quick-tap has nothing to store). Bounded + oldest-
 * dropped, so it can never grow without limit.
 *
 * Guarded by BOARD_HAS_TOUCH; a no-op elsewhere.
 */
#pragma once

#include "app_config.h"
#include "touch_gt911.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef BOARD_HAS_TOUCH

#define TOUCH_QUEUE_MAX 6

typedef struct {
    int32_t  x0, y0, x1, y1;
    uint32_t ms;
    uint32_t event_id;
    char     digest[24];   /* frame ETag displayed when touched (render_id ~16 hex) */
} touch_qentry_t;

/* Append a captured stroke + the ETag on screen at the time. Drops the oldest
 * entry if full. digest may be NULL/empty (then the entry is not queued -- the
 * server needs the frame digest to dispatch). */
void touch_queue_push(const touch_stroke_t *st, uint32_t event_id, const char *digest);

/* Number of queued (unsent) strokes. */
int  touch_queue_count(void);

/* Copy the oldest entry into *out without removing it. Returns false if empty. */
bool touch_queue_front(touch_qentry_t *out);

/* Remove the oldest entry (call after it is successfully dispatched). */
void touch_queue_pop(void);

#endif /* BOARD_HAS_TOUCH */
