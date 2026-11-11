#!/usr/bin/env python3
"""texpack_patches.py - the few textures whose upscale is redrawn by hand after texpack_upscale.py (tiny text the model cannot read).

    texpack_patches.py <extract dir (texpack_extract.py)> <pack dir (texpack_upscale.py output)>

Patches the PNGs in <pack dir> in place:
  585ca08f8a5250d3  title atlas: the copyright strip is upscaled with texpack_crisp.py instead of the model ("Righta Reaerved")
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


PATCHES = {"585ca08f8a5250d3": copyright_strip, "c6f05e9985eebe31": yes_no_labels}


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
