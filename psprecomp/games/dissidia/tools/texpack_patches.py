#!/usr/bin/env python3
"""texpack_patches.py - the few textures whose upscale is redrawn by hand after texpack_upscale.py (tiny text the model cannot read).

    texpack_patches.py <extract dir (texpack_extract.py)> <pack dir (texpack_upscale.py output)>

Patches the PNGs in <pack dir> in place:
  585ca08f8a5250d3  title atlas: the copyright strip is upscaled with texpack_crisp.py instead of the model ("Righta Reaerved")
  ea8da23a5f4cfe83, 01a8235b4aef00cb, 40c5a7313b7c99d5  customization-screen label atlases (TOTAL / BASE, ACCESSORY INFO, CATEGORY,
                    CURRENT, RANK, ITEM INFO, EQUIPMENT, SET A B C ...): each label is drawn again in Cinzel Bold in its original colour
  c6f05e9985eebe31  UI atlas: the grey "YES" (26x9 px cell) / "NO" (20x9 px cell) labels of every confirm prompt are drawn again in Cinzel Bold (OFL,
                    steam/fonts) at the same place and size; the model turned the blocky sprite into a blur
Ids that are missing from the folders are skipped. Needs numpy and Pillow.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
FONT = os.path.join(HERE, "..", "steam", "fonts", "Cinzel-Bold.ttf")
S = 4


def copyright_strip(orig, hd):
    import texpack_crisp as crisp
    b = (29, 497, 355, 512)                      # opaque white box x31..352, y499..511 with a 2 px margin
    k = hd.width // orig.width
    hd.paste(crisp.crisp(orig.crop(b), s=k), (b[0] * k, b[1] * k))
    return hd


def _fit_word(txt, box):
    f = ImageFont.truetype(FONT, 200)
    bb = f.getbbox(txt)
    m = Image.new("L", (bb[2] - bb[0], bb[3] - bb[1]), 0)
    ImageDraw.Draw(m).text((-bb[0], -bb[1]), txt, font=f, fill=255)
    x0, y0, x1, y1 = box
    m = m.resize((int(round((x1 - x0) * S)), int(round((y1 - y0) * S))), Image.LANCZOS)
    return np.asarray(m).astype(np.float32) / 255, int(round(x0 * S)), int(round(y0 * S))


def yes_no_labels(orig, hd):
    k = hd.width // orig.width
    assert k == S, "expects the 4x upscale"
    a = np.array(hd.convert("RGBA"))
    a[0:23 * S, 92 * S:128 * S] = [128, 128, 128, 0]
    cov = np.zeros(a.shape[:2], np.float32)
    for txt, box in (("YES", (97.6, 1.2, 122.4, 9.8)), ("NO", (97.4, 12.2, 116.6, 20.8))):
        m, X, Y = _fit_word(txt, box)
        ys = slice(max(0, Y), min(a.shape[0], Y + m.shape[0]))
        xs = slice(max(0, X), min(a.shape[1], X + m.shape[1]))
        cov[ys, xs] = np.maximum(cov[ys, xs], m[ys.start - Y:ys.stop - Y, xs.start - X:xs.stop - X])
    a[..., 3] = np.maximum(a[..., 3].astype(np.float32), cov * 255).astype(np.uint8)
    a[cov > 0, 0:3] = 128                         # the game tints the grey template (black on the light button, white on the blue one)
    return Image.fromarray(a, "RGBA")


# Small text baked into UI atlases: id -> [(string, (x0, y0, x1, y1) ink box in source pixels, inclusive)]. Found with `FT 1` and the
# atlas pictures; each box is one line (or one widely spaced word) of the original sprite sheet.
LABELS = {
    "ea8da23a5f4cfe83": [("TOTAL", (1, 104, 23, 109)), ("BASE", (35, 104, 53, 109)), ("ACCESSORY INFO", (2, 111, 60, 116)),
                         ("Equipment TRADE", (1, 118, 62, 124))],
    "01a8235b4aef00cb": [("SET", (1, 19, 19, 25)), ("A", (1, 27, 7, 33)), ("B", (10, 27, 15, 33)), ("C", (18, 27, 23, 33)),
                         ("D", (1, 47, 6, 53)), ("E", (9, 47, 15, 53))],
    "40c5a7313b7c99d5": [("ACCESSORY INFO", (2, 1, 60, 6)), ("CATEGORY", (2, 8, 41, 13)), ("CURRENT", (1, 16, 45, 23)),
                         ("TOTAL", (1, 26, 23, 31)), ("BASE", (35, 26, 53, 31)), ("RANK", (1, 33, 21, 38)),
                         ("ITEM INFO", (1, 40, 37, 45)), ("EQUIPMENT", (1, 48, 55, 56))],
}


def redraw_labels(orig, hd, labels):
    k = hd.width // orig.width
    o = np.array(orig.convert("RGBA"))
    a = np.array(hd.convert("RGBA"))
    for txt, (x0, y0, x1, y1) in labels:
        ink = o[y0:y1 + 1, x0:x1 + 1]
        solid = ink[ink[..., 3] > 150][:, :3]
        colour = solid.mean(0) if len(solid) else np.array([128, 128, 128])
        cy0, cy1 = int((y0 - 0.7) * k), int((y1 + 1.7) * k)
        cx0, cx1 = int((x0 - 0.7) * k), int((x1 + 1.7) * k)
        a[max(0, cy0):cy1, max(0, cx0):cx1] = [int(colour[0]), int(colour[1]), int(colour[2]), 0]
        f = ImageFont.truetype(FONT, 200)
        bb = f.getbbox(txt)
        m = Image.new("L", (bb[2] - bb[0], bb[3] - bb[1]), 0)
        ImageDraw.Draw(m).text((-bb[0], -bb[1]), txt, font=f, fill=255)
        w, h = (x1 + 1 - x0) * k, (y1 + 1 - y0) * k
        m = np.asarray(m.resize((w, h), Image.LANCZOS)).astype(np.float32) / 255
        X, Y = x0 * k, y0 * k
        reg = a[Y:Y + h, X:X + w]
        reg[..., 3] = np.maximum(reg[..., 3].astype(np.float32), m * 255).astype(np.uint8)
        reg[m > 0, 0:3] = colour.astype(np.uint8)
    return Image.fromarray(a, "RGBA")


PATCHES = {"585ca08f8a5250d3": copyright_strip, "c6f05e9985eebe31": yes_no_labels}
for _id, _labels in LABELS.items():
    PATCHES[_id] = (lambda labels: lambda orig, hd: redraw_labels(orig, hd, labels))(_labels)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    ext, pack = argv
    for tid, fn in PATCHES.items():
        src = os.path.join(ext, "src", tid + ".png")
        dst = os.path.join(pack, tid + ".png")
        if not (os.path.exists(src) and os.path.exists(dst)):
            print("skip", tid)
            continue
        out = fn(Image.open(src).convert("RGBA"), Image.open(dst).convert("RGBA"))
        out.save(dst, optimize=True)
        print("patched", tid)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
