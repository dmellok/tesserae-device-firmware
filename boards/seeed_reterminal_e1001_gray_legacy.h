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
 * THIS TARGET IS ON ITS WAY OUT. It used to be necessary: the claim was that
 * the panel is write-only (mono_spi sets miso_io_num = -1), so no image could
 * detect which glass it was talking to, and an operator had to flash both and
 * keep whichever refreshed. That claim was wrong. The UC8179 SDA line is
 * bidirectional and the controller reads back its own OTP, which records
 * whether a 4-gray table was burned in; mono_spi now bit-bangs that read at
 * port init (gray_probe_waveform) and picks the path itself, which is how the
 * stock Seeed firmware serves both batches from one image.
 *
 * So the plain gray target now covers this glass too. This one remains only to
 * pin the register-LUT path while the probe gets bench time, and should be
 * retired -- along with its device kind and catalog entry -- once a legacy
 * panel has been seen to probe correctly.
 *
 * Symptom guide, while both still exist:
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

/* Pin the register-LUT path instead of asking the panel. mono_spi maps this to
 * GRAY4_WAVEFORM 0, which skips the probe entirely; everything else --
 * geometry, palette, plane encoding, packing -- is inherited unchanged from the
 * gray header above. Drop this define (and this file) to let the probe decide. */
#define EPD_GRAY4_REG_LUTS 1
