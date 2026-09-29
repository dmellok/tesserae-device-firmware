/*
 * log_capture.c: esp_log tee into the RTC ring and the wake buffer, and the
 * upload. See log_capture.h; the ring, redaction and batch logic is in
 * log_ring.c.
 *
 * The hook runs in whatever task logged, so it allocates nothing, formats into
 * one static line buffer, and writes the buffers inside a short critical
 * section. The static buffer is claimed with a flag rather than held under the
 * lock: a line logged while another task is mid-format (or from inside the
 * hook itself) skips the capture, still reaches the UART, and is counted in a
 * "# ... N lines not captured" note ahead of the next captured line.
 */
#include "log_capture.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "log_ring.h"
#include "net_rest.h"
#include "rest_config.h"

static const char *TAG = "logcap";

/* RTC ring size. 3072 bytes is the tail of a typical wake (~50 bytes a line)
 * and leaves most of the 8 KB of RTC slow memory free; a board short of RTC
 * memory can set a smaller value in its header. */
#ifndef LOG_CAPTURE_RING_BYTES
#define LOG_CAPTURE_RING_BYTES 3072
#endif

/* Wake buffer: one whole timed wake is 5-10 KB, so 32 KB also covers a busy
 * one. Keeps the newest bytes when an always-on panel outlives it. */
#ifndef LOG_CAPTURE_WAKE_BYTES
#define LOG_CAPTURE_WAKE_BYTES (32 * 1024)
#endif

/* Without PSRAM the wake buffer would come out of internal RAM, which the
 * PSRAM-less boards need for the frame and the TLS session. None by default;
 * such a board uploads from the RTC ring. */
#ifndef LOG_CAPTURE_WAKE_INTERNAL_BYTES
#define LOG_CAPTURE_WAKE_INTERNAL_BYTES 0
#endif

/* Contract cap on one upload. */
#define LOG_UPLOAD_MAX (64 * 1024)

#define LOG_UPLOAD_TIMEOUT_MS 10000

RTC_NOINIT_ATTR static lr_rtc_hdr_t s_rtc;
RTC_NOINIT_ATTR static char         s_rtc_data[LOG_CAPTURE_RING_BYTES];

static lr_ring_t s_wake;          /* this boot; cap 0 when there is none */
static char     *s_wake_data;
static char     *s_carry;         /* unsent bytes from before this boot */
static uint32_t  s_carry_from;
static uint32_t  s_carry_len;
static uint32_t  s_boot_total;    /* stream offset this boot starts at */
static bool      s_ready;

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool           s_busy;     /* s_line / s_clean in use */
static uint32_t       s_skipped;  /* lines that missed the capture */
static vprintf_like_t s_prev;
static char           s_line[LR_LINE_MAX];
static char           s_clean[2 * LR_LINE_MAX];

/* Caller holds s_mux. */
static void append_locked(const char *p, size_t n)
{
    lr_ring_append(&s_rtc.ring, s_rtc_data, p, n);
    if (s_wake.cap) lr_ring_append(&s_wake, s_wake_data, p, n);
}

static int capture_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);

    bool mine = false;
    portENTER_CRITICAL_SAFE(&s_mux);
    if (!s_busy) {
        s_busy = true;
        mine = true;
    } else {
        s_skipped++;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    if (!mine) {
        va_end(copy);
        return ret;
    }

    int n = vsnprintf(s_line, sizeof s_line, fmt, copy);
    va_end(copy);
    size_t clean = 0;
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof s_line) {
            /* Cut short: keep it a line. */
            len = sizeof s_line - 1;
            s_line[len - 1] = '\n';
        }
        clean = lr_clean(s_line, len, s_clean, sizeof s_clean);
    }

    portENTER_CRITICAL_SAFE(&s_mux);
    uint32_t skipped = s_skipped;
    s_skipped = 0;
    portEXIT_CRITICAL_SAFE(&s_mux);
    char note[48];
    int nl = 0;
    if (skipped)
        nl = snprintf(note, sizeof note, "# ... %lu lines not captured\n",
                      (unsigned long)skipped);

    portENTER_CRITICAL_SAFE(&s_mux);
    if (nl > 0) append_locked(note, (size_t)nl);
    if (clean) append_locked(s_clean, clean);
    s_busy = false;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return ret;
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

static const char *wake_name(esp_sleep_wakeup_cause_t w)
{
    switch (w) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    case ESP_SLEEP_WAKEUP_EXT0:      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:     return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:  return "touchpad";
    case ESP_SLEEP_WAKEUP_ULP:       return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO:      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:      return "uart";
    default:                         return "other";
    }
}

/* PSRAM first, internal RAM as a fallback. */
static void *big_alloc(size_t n)
{
#if CONFIG_SPIRAM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

void log_capture_init(void)
{
    if (s_ready) return;

    bool kept = lr_rtc_begin_boot(&s_rtc, LOG_CAPTURE_RING_BYTES);
    s_boot_total = s_rtc.ring.total;

    /* Set aside what the previous wakes left unsent before this boot's lines
     * start overwriting it: the RTC ring is small enough that a wake fills it
     * well before its status POST. */
    uint32_t from = lr_rtc_unsent_from(&s_rtc);
    uint32_t unsent = s_boot_total - from;
    if (unsent) {
        s_carry = big_alloc(unsent);
        if (s_carry)
            s_carry_len = (uint32_t)lr_ring_read(&s_rtc.ring, s_rtc_data, from,
                                                 s_boot_total, s_carry, unsent,
                                                 &s_carry_from);
    }

    size_t wake_cap = 0;
#if CONFIG_SPIRAM
    s_wake_data = heap_caps_malloc(LOG_CAPTURE_WAKE_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_wake_data) wake_cap = LOG_CAPTURE_WAKE_BYTES;
#endif
#if LOG_CAPTURE_WAKE_INTERNAL_BYTES > 0
    if (!s_wake_data) {
        s_wake_data = malloc(LOG_CAPTURE_WAKE_INTERNAL_BYTES);
        if (s_wake_data) wake_cap = LOG_CAPTURE_WAKE_INTERNAL_BYTES;
    }
#endif
    lr_ring_reset(&s_wake, (uint32_t)wake_cap, s_boot_total);

    time_t now = time(NULL);
    uint32_t epoch = (now > 1700000000 && now < 2200000000LL) ? (uint32_t)now : 0;
    char hdr[160];
    int hn = lr_format_header(hdr, sizeof hdr, FW_VERSION, s_rtc.boot,
                              reset_name(esp_reset_reason()),
                              wake_name(esp_sleep_get_wakeup_cause()), epoch);
    if (hn > 0) {
        if ((size_t)hn >= sizeof hdr) {
            hn = (int)sizeof hdr - 1;
            hdr[hn - 1] = '\n';
        }
        portENTER_CRITICAL(&s_mux);
        append_locked(hdr, (size_t)hn);
        portEXIT_CRITICAL(&s_mux);
    }

    s_prev = esp_log_set_vprintf(capture_vprintf);
    s_ready = true;
    ESP_LOGI(TAG, "ring %u B (%s, %lu B unsent from earlier boots), wake buffer %u B",
             (unsigned)LOG_CAPTURE_RING_BYTES, kept ? "kept" : "fresh",
             (unsigned long)s_carry_len, (unsigned)wake_cap);
}

uint32_t log_capture_ring_bytes(void)
{
    return LOG_CAPTURE_RING_BYTES;
}

bool log_capture_upload(void)
{
    if (!s_ready || !rest_config_has_server()) return false;

    portENTER_CRITICAL(&s_mux);
    uint32_t to = s_rtc.ring.total;
    uint32_t from = s_rtc.uploaded;
    portEXIT_CRITICAL(&s_mux);

    /* This boot's bytes come from the wake buffer, or from the RTC ring on a
     * board without one. */
    const lr_ring_t *src = s_wake.cap ? &s_wake : &s_rtc.ring;
    const char *src_data = s_wake.cap ? s_wake_data : s_rtc_data;
    size_t cap = s_carry_len + src->cap + 3 * LR_MARKER_MAX;
    char *buf = big_alloc(cap);
    if (!buf) {
        ESP_LOGW(TAG, "upload skipped: no memory for a %u B batch", (unsigned)cap);
        return false;
    }
    lr_batch_t b;
    lr_batch_init(&b, buf, cap);

    /* Tail of earlier boots, from the copy taken at boot. */
    if (s_carry && s_carry_len) {
        uint32_t skip = lr_before(s_carry_from, from) ? from - s_carry_from : 0;
        if (skip < s_carry_len)
            lr_batch_add(&b, s_carry_from + skip, s_carry + skip,
                         s_carry_len - skip);
    }

    /* This boot, a chunk at a time so the critical section stays short. The
     * hook only ever overwrites the oldest bytes, so a chunk that lost its
     * front while we copied just starts later, and the batch marks the gap. */
    static char chunk[512];
    uint32_t pos = lr_before(from, s_boot_total) ? s_boot_total : from;
    while (lr_before(pos, to)) {
        uint32_t got_from;
        portENTER_CRITICAL(&s_mux);
        size_t got = lr_ring_read(src, src_data, pos, to, chunk, sizeof chunk,
                                  &got_from);
        portEXIT_CRITICAL(&s_mux);
        if (!got) break;
        lr_batch_add(&b, got_from, chunk, got);
        pos = got_from + (uint32_t)got;
    }

    size_t len = lr_batch_finish(&b, LOG_UPLOAD_MAX);
    if (len == 0) {
        free(buf);
        return true;
    }
    rest_status_t st = rest_post_log(buf, len, LOG_UPLOAD_TIMEOUT_MS);
    free(buf);
    if (st != REST_OK) {
        ESP_LOGW(TAG, "upload of %u B failed (%d); kept for the next request",
                 (unsigned)len, st);
        return false;
    }

    portENTER_CRITICAL(&s_mux);
    lr_rtc_mark_uploaded(&s_rtc, to);
    portEXIT_CRITICAL(&s_mux);
    /* Everything set aside at boot is on the server now. */
    free(s_carry);
    s_carry = NULL;
    s_carry_len = 0;
    ESP_LOGI(TAG, "uploaded %u B", (unsigned)len);
    return true;
}
