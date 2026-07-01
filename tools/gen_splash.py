#!/usr/bin/env python3
"""Generate a panel-native 4-bpp .bin splash for the Waveshare 13.3" Spectra 6.

Reads a square PNG logo (alpha composited onto white), Floyd-Steinberg dithers
the 1200x1600 canvas to the firmware's 6-colour palette
(0=black, 1=white, 2=yellow, 3=red, 5=blue, 6=green), and packs to the
panel's 4-bpp scanline-order format (high nibble = even col, low = odd col),
producing exactly 960,000 bytes.

The firmware embeds the output via CMake's EMBED_FILES and streams it
straight to the panel with the existing epd_display() path.

Usage:
    gen_splash.py --logo path/to/logo.png --out assets/splash_logo.bin \\
                  [--logo-size 600] [--logo-y CENTERED]
"""
import argparse
import os
import sys
from PIL import Image, ImageDraw, ImageFont
import numpy as np

try:
    import qrcode  # only required when --qr-data is used
except ImportError:
    qrcode = None

# Order matters: tried in turn until one loads. Helvetica.ttc ships with macOS
# and renders cleanly at the sizes we use; SFNS is the system UI font.
_FONT_CANDIDATES = [
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/SFNSDisplay.ttf",
    "/Library/Fonts/Arial.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in _FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size=size)
    # Last-resort bitmap font; small but readable.
    return ImageFont.load_default()

PANEL_W, PANEL_H = 1200, 1600

# (nibble, RGB) -- matches firmware app_config.h palette
PALETTE = [
    (0x0, (  0,   0,   0)),   # black
    (0x1, (255, 255, 255)),   # white
    (0x2, (255, 255,   0)),   # yellow
    (0x3, (255,   0,   0)),   # red
    (0x5, (  0,   0, 255)),   # blue
    (0x6, (  0, 255,   0)),   # green
]
PALETTE_RGB = np.array([rgb for _, rgb in PALETTE], dtype=np.float32)
PALETTE_NIBBLE = np.array([n for n, _ in PALETTE], dtype=np.uint8)


def make_canvas(logo_path: str, logo_size: int, logo_y: int) -> Image.Image:
    """Return a 1200x1600 RGB PIL image with the logo composited on white."""
    canvas = Image.new("RGB", (PANEL_W, PANEL_H), (255, 255, 255))

    logo = Image.open(logo_path).convert("RGBA")
    logo = logo.resize((logo_size, logo_size), Image.LANCZOS)
    bg = Image.new("RGB", logo.size, (255, 255, 255))
    bg.paste(logo, mask=logo.split()[3])

    x = (PANEL_W - logo_size) // 2
    canvas.paste(bg, (x, logo_y))
    return canvas


def overlay_labels(canvas: Image.Image, labels, y_top: int,
                   font_px: int = 44, line_gap_px: int = 18,
                   colour=(0, 0, 0)) -> int:
    """Centered black text lines stacked vertically. Returns y of the bottom
    of the last line so callers can stack further content under them."""
    font = load_font(font_px)
    draw = ImageDraw.Draw(canvas)
    y = y_top
    for line in labels:
        bbox = draw.textbbox((0, 0), line, font=font)
        text_w = bbox[2] - bbox[0]
        x = (PANEL_W - text_w) // 2
        draw.text((x, y), line, fill=colour, font=font)
        y += (bbox[3] - bbox[1]) + line_gap_px
    return y


def overlay_qr(canvas: Image.Image, data: str, target_px: int, y_top: int,
               quiet_zone: int = 4) -> int:
    """Render a QR code for `data` into a `target_px`-square area centered
    horizontally at `y_top` on the canvas, scaled with nearest-neighbor so
    every module aligns to an integer pixel grid (clean dither output)."""
    if qrcode is None:
        sys.exit("--qr-data requires the `qrcode` Python package "
                 "(pip install qrcode)")
    qr = qrcode.QRCode(
        version=None,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=1,
        border=quiet_zone,
    )
    qr.add_data(data)
    qr.make(fit=True)
    matrix = qr.get_matrix()    # list[list[bool]], includes quiet zone
    m_size = len(matrix)        # square; modules along one side incl. border
    module_px = target_px // m_size
    if module_px < 2:
        sys.exit(f"QR target_px={target_px} too small for {m_size}-module code "
                 f"(need at least {m_size * 2})")
    bmp_px = module_px * m_size

    # Render black-on-white at bmp_px resolution. Use a PIL Image so we can
    # paste straight onto the canvas.
    bmp = Image.new("L", (bmp_px, bmp_px), 255)
    pixels = bmp.load()
    for my in range(m_size):
        for mx in range(m_size):
            if matrix[my][mx]:
                for dy in range(module_px):
                    for dx in range(module_px):
                        pixels[mx * module_px + dx, my * module_px + dy] = 0

    x = (PANEL_W - bmp_px) // 2
    canvas.paste(bmp.convert("RGB"), (x, y_top))
    print(f"  qr: {m_size}x{m_size} modules ({m_size - 2 * quiet_zone} data + "
          f"{quiet_zone}-module border), {module_px}px/module, "
          f"final {bmp_px}x{bmp_px} at ({x},{y_top})")
    return y_top + bmp_px


def dither_to_nibbles(rgb: np.ndarray) -> np.ndarray:
    """Floyd-Steinberg dither rgb (H,W,3 float32) to the 6-colour palette.
    Returns an H x W uint8 array of palette nibble values."""
    h, w, _ = rgb.shape
    out = np.zeros((h, w), dtype=np.uint8)
    arr = rgb.copy()

    for y in range(h):
        for x in range(w):
            old = arr[y, x]
            dists = np.sum((PALETTE_RGB - old) ** 2, axis=1)
            idx = int(np.argmin(dists))
            new = PALETTE_RGB[idx]
            out[y, x] = PALETTE_NIBBLE[idx]
            err = old - new
            if x + 1 < w:
                arr[y, x + 1] += err * (7 / 16)
            if y + 1 < h:
                if x > 0:
                    arr[y + 1, x - 1] += err * (3 / 16)
                arr[y + 1, x] += err * (5 / 16)
                if x + 1 < w:
                    arr[y + 1, x + 1] += err * (1 / 16)
    return out


def pack_4bpp(nibbles: np.ndarray) -> bytes:
    """Pack H x W nibble array to panel-native 4-bpp scanline bytes:
    high nibble = even column, low = odd column."""
    hi = nibbles[:, 0::2].astype(np.uint8)
    lo = nibbles[:, 1::2].astype(np.uint8)
    packed = (hi << 4) | lo
    return packed.tobytes()


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--logo", required=True, help="source PNG (square logo)")
    p.add_argument("--out",  required=True, help="output .bin (panel-native 4bpp)")
    p.add_argument("--logo-size", type=int, default=600,
                   help="logo edge in panel pixels (default 600)")
    p.add_argument("--logo-y",    type=int, default=-1,
                   help="logo top Y in panel pixels (default: vertically centered)")
    p.add_argument("--qr-data",   help="bake a QR code for this string into the splash "
                                       "(e.g. 'WIFI:T:WPA;S:...;P:...;;')")
    p.add_argument("--qr-size",   type=int, default=700,
                   help="QR code target edge in panel pixels (default 700)")
    p.add_argument("--qr-y",      type=int, default=850,
                   help="QR top Y in panel pixels (default 850)")
    p.add_argument("--label",     action="append", default=[],
                   help="text line to render centered below the QR "
                        "(may be repeated to stack lines)")
    p.add_argument("--label-y",   type=int, default=-1,
                   help="top Y for the first label (default: 40px under QR)")
    p.add_argument("--label-px",  type=int, default=44,
                   help="label font size in panel pixels (default 44)")
    args = p.parse_args()

    if args.logo_size > PANEL_W:
        sys.exit(f"logo size {args.logo_size} > panel width {PANEL_W}")

    if args.logo_y < 0:
        args.logo_y = (PANEL_H - args.logo_size) // 2
    if args.logo_y + args.logo_size > PANEL_H:
        sys.exit(f"logo at y={args.logo_y} size {args.logo_size} exceeds panel height {PANEL_H}")

    print(f"compositing {args.logo} at ({(PANEL_W-args.logo_size)//2}, {args.logo_y}) "
          f"size {args.logo_size}x{args.logo_size} on {PANEL_W}x{PANEL_H} white...")
    canvas = make_canvas(args.logo, args.logo_size, args.logo_y)

    qr_bottom = args.qr_y
    if args.qr_data:
        print(f"baking QR for {args.qr_data!r}...")
        qr_bottom = overlay_qr(canvas, args.qr_data, args.qr_size, args.qr_y)

    if args.label:
        label_y = args.label_y if args.label_y >= 0 else qr_bottom + 40
        print(f"baking {len(args.label)} label line(s) at y={label_y}...")
        overlay_labels(canvas, args.label, label_y, font_px=args.label_px)

    print("Floyd-Steinberg dithering to the 6-colour palette...")
    nibbles = dither_to_nibbles(np.array(canvas, dtype=np.float32))

    packed = pack_4bpp(nibbles)
    expected = PANEL_W * PANEL_H // 2
    assert len(packed) == expected, f"got {len(packed)} bytes, expected {expected}"

    with open(args.out, "wb") as f:
        f.write(packed)
    print(f"wrote {args.out} ({len(packed)} bytes)")


if __name__ == "__main__":
    main()
