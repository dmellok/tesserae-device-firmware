/* Clock readiness before any TLS request.
 *
 * The panel validates server certificates, and with MBEDTLS_HAVE_TIME_DATE
 * enabled that check includes the certificate's validity window. The clock it
 * compares against comes from the PCF8563 at boot and, later, from each HTTP
 * `Date` header -- but the header only arrives after a successful handshake,
 * which is exactly the thing that needs the clock. A panel whose coin cell is
 * flat would otherwise sit at 1970, reject every certificate as "not yet
 * valid", and never recover.
 *
 * So: trust the RTC only when it seeds and reads plausibly, otherwise take one
 * bounded SNTP sync, otherwise make no TLS request this wake and keep the frame
 * that is already on the glass. Never "fix" a bad clock by relaxing the check.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 2025-01-01T00:00:00Z. Below this the clock was never set: the PCF8563 powers
 * up at 2000-01-01 and a wiped system clock reads 1970. */
#define TIME_READY_MIN_EPOCH 1735689600LL

/* 2100-01-01T00:00:00Z. A clock beyond this is not a working clock, and
 * adopting it would make every valid certificate look expired. */
#define TIME_READY_MAX_EPOCH 4102444800LL

/* One bounded wait for a cold wake. No retry loop: the radio is the most
 * expensive thing this device turns on, and a wake that cannot get time can
 * try again on its normal schedule. */
#define TIME_READY_TIMEOUT_MS 8000

typedef enum {
    TIME_READY_ALREADY_OK = 0,   /* the system clock was already plausible */
    TIME_READY_FROM_RTC,         /* the board RTC seeded it */
    TIME_READY_FROM_SNTP,        /* a bounded network sync did */
    TIME_READY_NO_TIME,          /* neither; no TLS request may be made */
} time_ready_result_t;

const char *time_ready_result_name(time_ready_result_t result);

/* True when `epoch` could be a real "now" for this device. */
bool time_ready_epoch_ok(int64_t epoch);

/* The sources, injected so the decision below can be exercised on a host with
 * no ESP-IDF, no radio and no RTC. Any of them may be NULL. */
typedef struct {
    int64_t (*now)(void);
    bool (*seed_from_rtc)(void);
    bool (*sync_from_network)(int timeout_ms);
} time_ready_sources_t;

/* Decide whether the clock can be trusted for certificate validation.
 * Returns true only when it can, and reports how the time was obtained.
 *
 * Order: an already-plausible clock is left alone (no I2C, no radio); a
 * successful RTC seed is trusted only if it actually moved the clock into the
 * permitted window; the network is tried once, bounded; otherwise it fails. */
bool time_ready_evaluate(const time_ready_sources_t *sources, int timeout_ms,
                         time_ready_result_t *result);

/* Firmware entry point: the real sources (system clock, PCF8563, SNTP). */
bool time_ready_ensure(int timeout_ms, time_ready_result_t *result);
