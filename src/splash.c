/*
 * splash.c: on-device procedural splash rendering. See splash.h.
 *
 * Draws into a panel-native packed-4bpp framebuffer (EPD_WIDTH x EPD_HEIGHT,
 * 2 px/byte, high nibble = even column) and paints it via epd_display(). One
 * implementation covers every panel geometry: portrait panels stack the splash
 * vertically; landscape panels place the logo + text on the left and the QR on
 * the right (per-orientation layouts below).
 *
 * Ported from tesserae-device-pico-bin/src/splash.c. Assets: public-domain
 * font8x8 (font8x8_basic.h), MIT qrcodegen (vendor/qrcodegen.c), Tesserae logo
 * (tesserae_logo.h).
 */
#include "splash.h"
#include "epd_driver.h"
#include "panel/epd_panel.h"
#include "app_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "qrcodegen.h"
#include "font8x8_basic.h"     /* char font8x8_basic[128][8] */
#include "tesserae_logo.h"     /* tesserae_logo[], TESSERAE_LOGO_W/H (nibble/px) */

static const char *TAG = "splash";

#define COL_BLK EPD_COL_BLACK
#define COL_WHT EPD_COL_WHITE

/* Render target for the current call. */
static uint8_t  *s_fb;
static const int s_W = EPD_WIDTH;
static const int s_H = EPD_HEIGHT;
static int       s_bpp = 4;   /* set per call from the active driver */

/* Per-call text for the portal-note / message splashes (set before render). */
static const char *s_portal_note;   /* NULL -> default "Setup mode" subtitle */
static const char *s_msg_title;
static const char *s_msg_body;

/* ---------- primitives (display coords: x across, y down) ---------- */

static inline void px(int x, int y, uint8_t c)
{
    if (x < 0 || y < 0 || x >= s_W || y >= s_H) return;
    if (s_bpp == 1) {
        /* Packed 1bpp, MSB = leftmost pixel; bit 1 = white. */
        size_t i = (size_t)y * (s_W / 8) + (size_t)(x >> 3);
        uint8_t bit = (uint8_t)(0x80 >> (x & 7));
        if (c == COL_WHT) s_fb[i] |= bit;
        else              s_fb[i] &= (uint8_t)~bit;
    } else if (s_bpp == 2) {
        /* Packed 2bpp (E1001 4-gray), 4 px/byte, MSB-first: bits 7-6 are the
         * leftmost pixel; 0b00 = black .. 0b11 = white. */
        size_t i = (size_t)y * (s_W / 4) + (size_t)(x >> 2);
        int shift = (3 - (x & 3)) * 2;
        s_fb[i] = (uint8_t)((s_fb[i] & (uint8_t)~(0x3 << shift)) |
                            (uint8_t)((c & 0x3) << shift));
    } else {
        /* Packed 4bpp, 2 px/byte, high nibble = even column. */
        size_t i = (size_t)y * (s_W / 2) + (size_t)(x >> 1);
        if (x & 1) s_fb[i] = (uint8_t)((s_fb[i] & 0xF0) | (c & 0x0F));
        else       s_fb[i] = (uint8_t)((s_fb[i] & 0x0F) | (uint8_t)(c << 4));
    }
}

static void fill_rect(int x, int y, int w, int h, uint8_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, c);
}

/* The logo bitmap is two-tone in Spectra palette values: 0x1 = the white
 * mosaic, 0x6 = the green mark. Map those to the active panel so it reads on any
 * depth: keep the Spectra palette on colour panels; on mono make the mark black;
 * on grayscale make the mark a dark tone (raw 0x1/0x6 are both near-black at
 * 4bpp gray, so the logo would otherwise vanish or smear). */
static uint8_t logo_color(uint8_t v)
{
    if (s_bpp == 1)      return (v == EPD_COL_WHITE) ? COL_WHT : COL_BLK;
    if (s_bpp == 2) {
        /* 2bpp panels: COL_WHT tells them apart -- 0x3 = 4-gray (dark-gray
         * mark), 0x1 = BWR (red mark, value 0x2). */
        if (v == 0x1) return COL_WHT;
        return (COL_WHT == 0x3) ? 0x1 : 0x2;
    }
    if (COL_WHT == 0x1)  return v;                      /* Spectra: raw (green mark) */
    return (v == 0x1) ? COL_WHT : 0x3;                  /* grayscale: white / dark-gray mark */
}

/* Logo, nearest-neighbour scaled to `size` px square, top-left at (cx-size/2, top). */
static void blit_logo(int cx, int top, int size)
{
    int x0 = cx - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int sy = dy * TESSERAE_LOGO_H / size;
        for (int dx = 0; dx < size; dx++) {
            int sx = dx * TESSERAE_LOGO_W / size;
            px(x0 + dx, top + dy, logo_color(tesserae_logo[sy * TESSERAE_LOGO_W + sx]));
        }
    }
}

/* ---------- text (font8x8, LSB = leftmost) ---------- */

static void draw_char(int x, int y, char ch, int s, uint8_t c)
{
    const char *g = font8x8_basic[(unsigned char)ch & 0x7F];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if ((g[row] >> col) & 1) fill_rect(x + col * s, y + row * s, s, s, c);
}

static int text_w(const char *str, int s) { return (int)strlen(str) * 8 * s; }

/* Draw `str` at scale `s`, horizontally centered within [x0, x0+w). */
static void draw_text_in(int x0, int w, int y, const char *str, int s, uint8_t c)
{
    int x = x0 + (w - text_w(str, s)) / 2;
    for (const char *p = str; *p; p++, x += 8 * s) draw_char(x, y, *p, s, c);
}

/* Word-wrap `text` into lines that fit `w`, each centered in [x0, x0+w),
 * starting at `y`. Returns the y past the last line. Breaks on spaces; a word
 * longer than one line is hard-cut. */
static int draw_paragraph(int x0, int w, int y, const char *text, int s, uint8_t c)
{
    const int line_h  = 8 * s + 4 * s;                 /* glyph + interline gap */
    int max_chars = w / (8 * s); if (max_chars < 1) max_chars = 1;
    char line[96];
    int  ll = 0;
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;                          /* skip spaces */
        const char *we = p; while (*we && *we != ' ') we++;
        int wl = (int)(we - p);
        if (wl == 0) break;
        if (wl > max_chars) wl = max_chars;             /* hard-cut overlong word */
        if (ll == 0) {
            memcpy(line, p, wl); ll = wl;
        } else if (ll + 1 + wl <= max_chars) {
            line[ll++] = ' '; memcpy(line + ll, p, wl); ll += wl;
        } else {
            line[ll] = '\0';
            draw_text_in(x0, w, y, line, s, c); y += line_h;
            memcpy(line, p, wl); ll = wl;
        }
        p = we;
    }
    if (ll) { line[ll] = '\0'; draw_text_in(x0, w, y, line, s, c); y += line_h; }
    return y;
}

/* ---------- QR ---------- */

static void draw_qr(const uint8_t *qr, int x, int y, int scale)
{
    int n = qrcodegen_getSize(qr);
    fill_rect(x - 4 * scale, y - 4 * scale, (n + 8) * scale, (n + 8) * scale, COL_WHT);
    for (int qy = 0; qy < n; qy++)
        for (int qx = 0; qx < n; qx++)
            if (qrcodegen_getModule(qr, qx, qy))
                fill_rect(x + qx * scale, y + qy * scale, scale, scale, COL_BLK);
}

/* ---------- render harness ---------- */

/* Allocate the framebuffer (PSRAM), clear to white, run `draw`, paint, free. */
static esp_err_t render_and_paint(void (*draw)(void), const char *label)
{
    esp_err_t err = epd_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "epd_port_init failed (%d); skipping %s splash", err, label);
        return err;
    }

    s_bpp = epd_active_driver()->info.bpp;

    s_fb = heap_caps_malloc(EPD_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "OOM allocating %u-byte splash buffer", (unsigned)EPD_BUF_BYTES);
        return ESP_ERR_NO_MEM;
    }
    /* White background: 1bpp -> all bits set; 2bpp -> COL_WHT in all four
     * 2-bit slots (0xFF on 4-gray, 0x55 on BWR); 4bpp -> packed nibbles. */
    memset(s_fb, (s_bpp == 1) ? 0xFF
                : (s_bpp == 2) ? (uint8_t)(COL_WHT * 0x55)
                : (uint8_t)((COL_WHT << 4) | COL_WHT), EPD_BUF_BYTES);

    draw();

    ESP_LOGI(TAG, "painting %s splash (%dx%d, ~30 s)...", label, s_W, s_H);
    epd_init();
    epd_display(s_fb);
    epd_sleep();

    free(s_fb);
    s_fb = NULL;
    return ESP_OK;
}

/* ---------- shared QR builder ---------- */

/* Encode the SoftAP WiFi-join QR. Returns the module count (0 on failure) and
 * fills `qrbuf` (must be qrcodegen_BUFFER_LEN_FOR_VERSION(6)). */
static int build_ap_qr(uint8_t *qrbuf)
{
    char wifi[96];
    if (PROVISION_AP_PASS[0])
        snprintf(wifi, sizeof wifi, "WIFI:S:%s;T:WPA;P:%s;;",
                 PROVISION_AP_SSID, PROVISION_AP_PASS);
    else
        snprintf(wifi, sizeof wifi, "WIFI:S:%s;T:nopass;;", PROVISION_AP_SSID);

    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    if (!qrcodegen_encodeText(wifi, tmp, qrbuf, qrcodegen_Ecc_MEDIUM,
                              1, 6, qrcodegen_Mask_AUTO, true))
        return 0;
    return qrcodegen_getSize(qrbuf);
}

static const char *k_url = "Open  http://192.168.4.1";

/* ---------- cold-boot logo ---------- */

static void draw_logo(void)
{
    int side = (s_W < s_H) ? s_W : s_H;
    int logo = side / 3;
    int s    = side / 240; if (s < 2) s = 2;
    int th   = logo + logo / 6 + 8 * s;               /* logo + gap + title */
    int y    = (s_H - th) / 2;
    blit_logo(s_W / 2, y, logo);                       y += logo + logo / 6;
    draw_text_in(0, s_W, y, "Tesserae", s, COL_BLK);
}

/* ---------- provisioning splash ---------- */

static void draw_portal_portrait(void)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    int qn = build_ap_qr(qr);

    int s     = s_W / 240; if (s < 2) s = 2;           /* body scale */
    int ts    = 2 * s;                                  /* title scale */
    int logo  = s_W / 3;
    int gap   = 8 * s;

    int qscale = qn ? (s_H / 4) / (qn + 8) : 0; if (qscale < 1) qscale = 1;
    if (qn && qn * qscale > 400) qscale = 400 / qn;    /* cap ~400 px */
    int qpix   = qn ? qn * qscale : 0;
    int qz     = qn ? 4 * qscale : 0;

    int total = logo + gap + 8 * ts + gap + 8 * s + gap + 8 * s + gap + 8 * s
                + (qn ? gap + qz + qpix : 0);
    int y = (s_H - total) / 2 - s_H / 14; if (y < gap) y = gap;

    char line_ssid[64];
    snprintf(line_ssid, sizeof line_ssid, "Wi-Fi:  %s", PROVISION_AP_SSID);

    blit_logo(s_W / 2, y, logo);                              y += logo + gap;
    draw_text_in(0, s_W, y, "Tesserae", ts, COL_BLK);         y += 8 * ts + gap;
    draw_text_in(0, s_W, y, s_portal_note ? s_portal_note : "Setup mode", s, COL_BLK);
                                                              y += 8 * s + gap;
    draw_text_in(0, s_W, y, line_ssid, s, COL_BLK);           y += 8 * s + gap;
    draw_text_in(0, s_W, y, k_url, s, COL_BLK);               y += 8 * s + gap;
    if (qn) { y += qz; draw_qr(qr, (s_W - qpix) / 2, y, qscale); }
}

/* Landscape: logo + text on the left half, QR centered on the right half. */
static void draw_portal_landscape(void)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    int qn = build_ap_qr(qr);

    const int halfW = s_W / 2;
    const int lm    = halfW / 8;                        /* left inset (nudges the
                                                          column right of edge) */
    const int lw    = halfW - lm;                       /* left-column text width */

    /* Body scale sized so the longest line (~26 chars) fits the column -- height-
     * based scaling made the URL overflow on the big 1872-wide E1003. */
    int s    = (lw * 9 / 10) / (26 * 8); if (s < 2) s = 2;
    int ts   = 2 * s;
    int gap  = 8 * s;
    int logo = (s_H * 2) / 5;                           /* ~40% of height */
    if (logo > lw) logo = lw;

    int block = logo + gap + 8 * ts + gap + 8 * s + gap + 8 * s + gap + 8 * s;
    int y = (s_H - block) / 2; if (y < gap) y = gap;

    char line_ssid[64];
    snprintf(line_ssid, sizeof line_ssid, "Wi-Fi:  %s", PROVISION_AP_SSID);

    int cx = lm + lw / 2;                               /* left-column centre */
    blit_logo(cx, y, logo);                             y += logo + gap;
    draw_text_in(lm, lw, y, "Tesserae", ts, COL_BLK);   y += 8 * ts + gap;
    draw_text_in(lm, lw, y, s_portal_note ? s_portal_note : "Setup mode", s, COL_BLK);
                                                        y += 8 * s + gap;
    draw_text_in(lm, lw, y, line_ssid, s, COL_BLK);     y += 8 * s + gap;
    draw_text_in(lm, lw, y, k_url, s, COL_BLK);

    /* Right column: QR centered, clamped so it doesn't dominate a large panel. */
    if (qn) {
        int qscale = ((s_H * 3) / 4) / (qn + 8); if (qscale < 1) qscale = 1;
        if (qn * qscale > 400) qscale = 400 / qn;       /* cap ~400 px */
        int qpix   = qn * qscale;
        int qx = halfW + (halfW - qpix) / 2;
        int qy = (s_H - qpix) / 2;
        draw_qr(qr, qx, qy, qscale);
    }
}

static void draw_portal(void)
{
    if (s_W > s_H) draw_portal_landscape();
    else           draw_portal_portrait();
}

/* ---------- message (logo + title + wrapped body, no QR) ---------- */

static void draw_message(void)
{
    int side = (s_W < s_H) ? s_W : s_H;
    int logo = side / 4;
    int s    = side / 240; if (s < 2) s = 2;
    int ts   = 2 * s;
    int gap  = 8 * s;
    int x0   = s_W / 10;
    int w    = s_W - 2 * x0;

    int y = s_H / 6;
    blit_logo(s_W / 2, y, logo);                                   y += logo + gap;
    if (s_msg_title && *s_msg_title) {
        draw_text_in(0, s_W, y, s_msg_title, ts, COL_BLK);         y += 8 * ts + gap;
    }
    if (s_msg_body && *s_msg_body) {
        draw_paragraph(x0, w, y, s_msg_body, s, COL_BLK);
    }
}

/* ---------- public API ---------- */

esp_err_t splash_show_logo(void)   { s_portal_note = NULL; return render_and_paint(draw_logo,   "logo"); }
esp_err_t splash_show_portal(void) { s_portal_note = NULL; return render_and_paint(draw_portal, "portal"); }

esp_err_t splash_show_portal_note(const char *subtitle)
{
    s_portal_note = subtitle;
    return render_and_paint(draw_portal, "portal-note");
}

esp_err_t splash_show_message(const char *title, const char *body)
{
    s_msg_title = title;
    s_msg_body  = body;
    return render_and_paint(draw_message, "message");
}
