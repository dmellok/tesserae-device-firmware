/*
 * On-device procedural boot splashes.
 *
 * Rendered at runtime into a panel-native packed-4bpp framebuffer sized to the
 * active panel (EPD_WIDTH x EPD_HEIGHT), then painted via epd_display(). Because
 * it draws to whatever geometry the board reports, one implementation works for
 * every panel -- portrait (1200x1600) and landscape (800x480) alike. Monochrome
 * (black on white) so it is correct on any Spectra/colour palette.
 *
 * Uses the public-domain font8x8 (font8x8_basic.h), the MIT qrcodegen
 * (vendor/qrcodegen.c), and the Tesserae logo bitmap (tesserae_logo.h).
 *
 * Each call brings the panel up, refreshes (~20-30 s), and puts it back to
 * sleep -- safe to invoke with the panel off or already initialised
 * (epd_port_init is idempotent).
 */
#pragma once

#include "esp_err.h"

/* Tesserae logo + wordmark, centered on white. Shown on cold boot when WiFi
 * creds are present, so the user knows the device just booted. */
esp_err_t splash_show_logo(void);

/* Provisioning splash: logo, "Setup mode", the SoftAP name + portal URL, and a
 * WPA-format QR that joins the setup AP when scanned. Portrait panels stack it
 * vertically; landscape panels place the logo/text on the left and the QR on
 * the right. Shown right before the captive portal comes up. The QR encodes the
 * SAME credentials the SoftAP advertises (PROVISION_AP_SSID/PASS). */
esp_err_t splash_show_portal(void);

/* Same provisioning splash, but the "Setup mode" subtitle is replaced by a
 * short reason (e.g. "Wi-Fi didn't connect", "Can't reach the server") so a
 * user sent back to the portal after a failed connect/onboard knows why and
 * what to fix. Keep `subtitle` short (fits one ~26-char line). */
esp_err_t splash_show_portal_note(const char *subtitle);

/* Logo + a bold title + a word-wrapped body, centered. No QR. Used for connect
 * feedback: "Connected! / Waiting for your first frame", "Almost done /
 * Approve this device in Tesserae". */
esp_err_t splash_show_message(const char *title, const char *body);
