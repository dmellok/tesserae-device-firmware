/*
 * ota_report.h -- OTA lifecycle state reporting (docs/ota/contract.md,
 * "State reporting").
 *
 * The device persists the latest OTA attempt state in NVS and reports it in
 * the `ota` object of every /status heartbeat (phase / reason / target_fw /
 * attempt_id). The server dedups repeated identical reports, so the firmware
 * just keeps sending the current state; a new attempt overwrites it. State
 * lives in NVS, not RTC memory, because it must survive the A/B slot switch
 * (two different images do not share an RTC memory layout).
 *
 * Guarded by TESSERAE_OTA_CAPABILITY_ENABLED; a no-op elsewhere.
 */
#pragma once

#include "app_config.h"

#if TESSERAE_OTA_CAPABILITY_ENABLED

#include "cJSON.h"

/* Contract phases (subset this single-wake firmware can be in). `idle` is
 * represented by having no stored report. */
typedef enum {
    OTA_REPORT_IDLE = 0,
    OTA_REPORT_DOWNLOADING,     /* streaming into the inactive slot          */
    OTA_REPORT_PENDING_CONFIRM, /* new image selected, reboot imminent/next  */
    OTA_REPORT_CONFIRMED,       /* first boot passed, slot marked valid      */
    OTA_REPORT_REJECTED,        /* descriptor refused before any download    */
    OTA_REPORT_FAILED,          /* attempt failed during/after download      */
    OTA_REPORT_ROLLED_BACK,     /* first-boot gate failed, previous slot back */
} ota_report_phase_t;

/* Record the current attempt state and persist it immediately (the very next
 * event may be a reboot). reason uses the contract vocabulary; "" for
 * in-progress phases. target_fw is the descriptor's fw_version; attempt_id an
 * opaque stable id for the attempt (we use the first 6 hex chars of the
 * descriptor sha256). */
void ota_report_set(ota_report_phase_t phase, const char *reason,
                    const char *target_fw, const char *attempt_id);

/* Resolve a cross-reboot pending_confirm into its terminal state. Call once
 * per boot after the rollback gate has run: pending_confirm + running the
 * target version -> confirmed/ok; pending_confirm + running anything else ->
 * rolled_back/boot_failed (the bootloader reverted the slot). */
void ota_report_resolve_boot(const char *running_fw);

/* Add the report fields (phase/reason/target_fw/attempt_id) to the heartbeat's
 * `ota` object. Adds nothing when idle -- the schema field alone means idle. */
void ota_report_fill(cJSON *ota_obj);

#else /* !TESSERAE_OTA_CAPABILITY_ENABLED */

#define ota_report_resolve_boot(fw) ((void)0)

#endif /* TESSERAE_OTA_CAPABILITY_ENABLED */
