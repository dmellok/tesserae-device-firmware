/* touch_queue.c -- RTC-backed queue of captured touch strokes. See touch_queue.h. */

#include "touch_queue.h"

#ifdef BOARD_HAS_TOUCH

#include <string.h>
#include "esp_attr.h"

/* Lives in RTC slow memory so it survives deep sleep. A magic word tells a
 * retained queue apart from power-on garbage. */
#define TOUCH_QUEUE_MAGIC 0x54515545u   /* 'TQUE' */
RTC_NOINIT_ATTR static uint32_t        s_magic;
RTC_NOINIT_ATTR static touch_qentry_t  s_q[TOUCH_QUEUE_MAX];
RTC_NOINIT_ATTR static uint8_t         s_head;   /* index of the oldest entry */
RTC_NOINIT_ATTR static uint8_t         s_count;

static void ensure_init(void)
{
    if (s_magic != TOUCH_QUEUE_MAGIC) {   /* power-on / garbage: start empty */
        s_magic = TOUCH_QUEUE_MAGIC;
        s_head = 0;
        s_count = 0;
    }
}

void touch_queue_push(const touch_stroke_t *st, uint32_t event_id, const char *digest)
{
    ensure_init();
    if (!st || !st->valid || !digest || !digest[0]) return;

    uint8_t slot = (uint8_t)((s_head + s_count) % TOUCH_QUEUE_MAX);
    if (s_count == TOUCH_QUEUE_MAX) {
        s_head = (uint8_t)((s_head + 1) % TOUCH_QUEUE_MAX);   /* drop oldest */
        slot = (uint8_t)((s_head + s_count - 1) % TOUCH_QUEUE_MAX);
    } else {
        s_count++;
    }

    touch_qentry_t *e = &s_q[slot];
    e->x0 = st->x0; e->y0 = st->y0; e->x1 = st->x1; e->y1 = st->y1;
    e->ms = st->ms; e->event_id = event_id;
    /* Strip surrounding quotes off the ETag; the server tolerates either. */
    const char *d = digest;
    size_t n = strlen(d);
    if (n >= 2 && d[0] == '"' && d[n - 1] == '"') { d++; n -= 2; }
    if (n >= sizeof e->digest) n = sizeof e->digest - 1;
    memcpy(e->digest, d, n);
    e->digest[n] = '\0';
}

int touch_queue_count(void)
{
    ensure_init();
    return s_count;
}

bool touch_queue_front(touch_qentry_t *out)
{
    ensure_init();
    if (s_count == 0 || !out) return false;
    *out = s_q[s_head];
    return true;
}

void touch_queue_pop(void)
{
    ensure_init();
    if (s_count == 0) return;
    s_head = (uint8_t)((s_head + 1) % TOUCH_QUEUE_MAX);
    s_count--;
}

#endif /* BOARD_HAS_TOUCH */
