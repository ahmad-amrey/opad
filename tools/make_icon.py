#!/usr/bin/env python3
"""Cuts the program icon out of the OPAD logo: python tools/make_icon.py <opad_logo.png>

The logo is the cube mark followed by the "OPAD" wordmark on a transparent background. The icon is always the mark
alone:
  app/res/opad.ico        16..256 px, the exe icon (app/res/opad.rc.in)
  app/res/opad-<n>.png    the window icon (app/res.qrc, icons::appIcon)
The cube's inner face is a hole in the logo (white only on a white page); the icon fills it white so the mark looks
the same on a dark taskbar.
The outputs are committed; run this again only when the logo changes. Needs Pillow and numpy.
"""
import struct
import sys
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
PNG_SIZES = [16, 24, 32, 48, 64, 256]
WORK = 1024  # the mark is squared up at this size before it is reduced


def fill_holes(mark):
    """Transparent areas enclosed by the mark (the cube's inner face) become white; the outside stays transparent."""
    padded = Image.new("RGBA", (mark.width + 2, mark.height + 2), (0, 0, 0, 0))
    padded.paste(mark, (1, 1))
    a = np.asarray(padded).astype(np.float32)
    # .copy(): an image that still shares the array's memory is read-only and floodfill silently does nothing
    open_px = Image.fromarray(np.where(a[:, :, 3] < 250, 255, 0).astype(np.uint8)).copy()
    ImageDraw.floodfill(open_px, (0, 0), 128)  # 128 = reachable from outside
    hole = np.asarray(open_px) == 255
    alpha = a[:, :, 3:4] / 255.0
    a[:, :, :3] = np.where(hole[:, :, None], a[:, :, :3] * alpha + 255.0 * (1.0 - alpha), a[:, :, :3])
    a[:, :, 3] = np.where(hole, 255.0, a[:, :, 3])
    return Image.fromarray((a + 0.5).astype(np.uint8), "RGBA").crop((1, 1, mark.width + 1, mark.height + 1))


def mark_box(rgba):
    """Bounding box of the first run of occupied columns: the mark, left of the wordmark."""
    solid = np.asarray(rgba)[:, :, 3] > 128
    cols = np.flatnonzero(solid.any(axis=0))
    gaps = np.flatnonzero(np.diff(cols) > rgba.width // 100)
    last = cols[gaps[0]] if len(gaps) else cols[-1]
    rows = np.flatnonzero(solid[:, cols[0]:last + 1].any(axis=1))
    return int(cols[0]) - 2, int(rows[0]) - 2, int(last) + 3, int(rows[-1]) + 3


def squared(mark):
    side = max(mark.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(mark, ((side - mark.width) // 2, (side - mark.height) // 2))
    return canvas.resize((WORK, WORK), Image.LANCZOS)


def reduced(img, size):
    """Resize with premultiplied alpha, so the transparent surround never bleeds into the edge."""
    a = np.asarray(img).astype(np.float32) / 255.0
    a[:, :, :3] *= a[:, :, 3:4]
    chans = [Image.fromarray(a[:, :, c]).resize((size, size), Image.LANCZOS if size >= 48 else Image.BOX) for c in range(4)]
    b = np.clip(np.stack([np.asarray(c) for c in chans], axis=2), 0.0, 1.0)
    alpha = b[:, :, 3:4]
    b[:, :, :3] = np.where(alpha > 1e-4, b[:, :, :3] / np.maximum(alpha, 1e-4), 0.0)
    return Image.fromarray((np.clip(b, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "RGBA")


def png_bytes(img):
    buf = BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def dib(img):
    """32-bit BGRA bitmap as an .ico stores it: doubled height, bottom-up, followed by an (empty) AND mask."""
    size = img.width
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = np.asarray(img)[::-1, :, [2, 1, 0, 3]].tobytes()
    mask = bytes(((size + 31) // 32) * 4 * size)
    return header + pixels + mask


def ico(images):
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return out + b"".join(data for _, data in images)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    res = Path(__file__).resolve().parent.parent / "app" / "res"
    res.mkdir(parents=True, exist_ok=True)
    logo = Image.open(sys.argv[1]).convert("RGBA")
    box = mark_box(logo)
    mark = squared(fill_holes(logo.crop(box)))
    images = []
    for size in sorted(set(ICO_SIZES + PNG_SIZES)):
        img = reduced(mark, size)
        if size in PNG_SIZES:
            (res / f"opad-{size}.png").write_bytes(png_bytes(img))
        if size in ICO_SIZES:
            images.append((size, png_bytes(img) if size == 256 else dib(img)))  # 256 px is stored as PNG
    (res / "opad.ico").write_bytes(ico(images))
    print(f"mark at {box} of {logo.size}; wrote {res / 'opad.ico'} and opad-<{','.join(map(str, PNG_SIZES))}>.png")


if __name__ == "__main__":
    main()
