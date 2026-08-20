/*
 * Board variant: Seeed reTerminal E1001, 4-level GRAYSCALE for LEGACY glass.
 *
 * FUNCTIONALLY IDENTICAL to seeed_reterminal_e1001_gray.h. It exists now only
 * to keep an OTA lineage alive, not because it drives the panel differently.
 *
 * It used to. The two batches of this glass need different waveforms -- newer
 * panels carry a built-in OTP 4-gray table, older ones must have register LUTs
 * uploaded -- and the claim was that nothing could tell them apart, because the
 * panel is write-only (mono_spi sets miso_io_num = -1). So there were two
 * targets and an operator flashed both to see which refreshed. The claim was
 * wrong: the UC8179 SDA line is bidirectional and the controller reads back its
 * own OTP, which records whether a 4-gray table was burned in. mono_spi bit-bangs
 * that read at port init (gray_probe_waveform) and picks the path itself, which
 * is how the stock Seeed firmware serves both batches from one image. Confirmed
 * on a user's legacy panel 2026-08-20: the probe read it correctly and selected
 * the register-LUT path unassisted.
 *
 * So this target no longer pins anything. It probes, exactly like the plain gray
 * build, and the only differences left are the device kind and the probe-failure
 * fallback below.
 *
 * WHY IT IS STILL HERE. Kind is the OTA lineage: release.yml stages each target's
 * signing input as "<kind>.app.bin". This kind has shipped since v1.12.0, so
 * deleting the target would mean nothing is ever published under it again and any
 * field device keyed to it silently stops receiving updates -- sitting on an old
 * release with no error anywhere. It has to outlive the deletion.
 *
 * RETIREMENT PATH. Keep building this until no device reports the legacy kind,
 * then drop the env, this header, and the catalog entry. Because the two images
 * now behave the same, a legacy-kind device that takes an OTA of THIS target
 * gets the probe and the corrected waveform, so the migration needs no
 * intervention from the user. Stop offering it in the flasher immediately
 * though -- new devices have no reason to pick it.
 *
 * SERVER DEPENDENCY: Tesserae needs a hardware-catalog entry for
 * seeed_reterminal_e1001_gray_legacy -- a copy of
 * hardware/seeed/reterminal_e1001_gray.json with this id. Same protocol
 * (esp32_bw_client), same panel.gamut (gray_4), same renderers override
 * (esp32_gray2_bin), because the frame bytes are identical. Without it the
 * panel pairs but never receives a correctly packed frame. That entry must stay
 * until the kind is fully retired.
 */
#pragma once

#include "seeed_reterminal_e1001_gray.h"

/* Separate OTA/render lineage; see the note above on why this is not shared. */
#undef  TESSERAE_DEVICE_KIND
#define TESSERAE_DEVICE_KIND  "seeed_reterminal_e1001_gray_legacy"

/* If the probe cannot answer, use register LUTs rather than the OTP waveform.
 *
 * This is the one substantive difference from the plain gray build, and it is
 * the reason this target can safely take the probe at all. Anything running this
 * firmware is on glass that was already established to have no built-in 4-gray
 * table -- that is why its owner is on this target. Falling back to OTP on a
 * failed read would break a device that works today; falling back to register
 * LUTs is what it was already doing. So the probe can only improve matters here,
 * never regress them. */
#define GRAY4_WAVEFORM_FALLBACK 0
