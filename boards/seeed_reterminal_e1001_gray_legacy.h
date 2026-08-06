/*
 * Board variant: Seeed reTerminal E1001, 4-level GRAYSCALE for LEGACY glass.
 *
 * Identical to seeed_reterminal_e1001_gray.h in every respect the SERVER can
 * see -- same 800x480 panel, same 2bpp/96000-byte wire format, same gamut --
 * and differs only in how mono_spi drives the glass:
 *
 *   gray        (default)  GEN2 built-in OTP 4-gray waveform, selected by
 *                          fixing the temperature index (CCSET TSFIX +
 *                          TSSET 0x5F). PSR stays 0x1F.
 *   gray_legacy (this)     Register LUTs uploaded to R20-R25, PSR 0x3F.
 *
 * Why both exist: the OTP waveform is present only on newer batches of this
 * glass. On the production E1001 benched 2026-07-23 the register-LUT path never
 * started a refresh at all, which is why the OTP path is the default. The
 * converse is the reason for this variant -- older glass has no OTP 4-gray
 * table, so it needs the LUTs uploaded.
 *
 * The panel is WRITE-ONLY (mono_spi sets miso_io_num = -1), so there is no
 * controller id to read back and no way for one image to detect which glass it
 * is talking to before driving it. Hence two build targets rather than runtime
 * detection: the operator picks by observing which one refreshes.
 *
 * Symptom guide for choosing:
 *   - nothing happens at all, no refresh  -> wrong path for this glass
 *   - refreshes but ghosts / flashes erratically -> usually NOT the waveform
 *     path; suspect batch variance, drive voltages, or the panel itself
 *
 * The KIND differs from the plain gray build even though the rendered bytes are
 * byte-identical, because kind is also the OTA lineage: release.yml stages the
 * signing input as "<kind>.app.bin" and merges all targets into one directory,
 * so two targets sharing a kind would overwrite each other's signing input --
 * and a GEN2 panel could be offered the legacy image, which does not refresh on
 * it. Distinct kinds keep the two update streams separate.
 *
 * SERVER DEPENDENCY: Tesserae needs a hardware-catalog entry for
 * seeed_reterminal_e1001_gray_legacy -- a copy of
 * hardware/seeed/reterminal_e1001_gray.json with this id. Same protocol
 * (esp32_bw_client), same panel.gamut (gray_4), same renderers override
 * (esp32_gray2_bin), because the frame bytes are identical. Without it the
 * panel pairs but never receives a correctly packed frame.
 */
#pragma once

#include "seeed_reterminal_e1001_gray.h"

/* Separate OTA/render lineage; see the note above on why this is not shared. */
#undef  TESSERAE_DEVICE_KIND
#define TESSERAE_DEVICE_KIND  "seeed_reterminal_e1001_gray_legacy"

/* Upload register LUTs instead of using the built-in OTP 4-gray waveform.
 * mono_spi keys its init sequence off this (see the EPD_GRAY4_REG_LUTS
 * branch); everything else -- geometry, palette, plane encoding, packing --
 * is inherited unchanged from the gray header above. */
#define EPD_GRAY4_REG_LUTS 1
