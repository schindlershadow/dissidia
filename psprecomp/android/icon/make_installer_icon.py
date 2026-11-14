#!/usr/bin/env python3
"""Draw the setup app's icon: the game's crystal (make_icon.py) turned teal, with a "download into a tray" badge, so the installer is not mistaken for the game.
Writes installer_512.png, setup_res/mipmap-*/ic_launcher.png (the Android setup app) and, via games/dissidia/icon/embed_icon.py, the desktop wizard's icon files.
  python3 make_installer_icon.py          (needs Pillow)
"""
import os
from PIL import Image, ImageDraw, ImageFilter

import make_icon

HERE = os.path.dirname(os.path.abspath(__file__))
S = make_icon.S


def make():
    img = make_icon.make().convert("HSV")
    h, s, v = img.split()
    h = h.point(lambda x: (x - 62) % 256)   # blue / violet -> teal / green
    img = Image.merge("HSV", (h, s, v)).convert("RGBA")
    d = ImageDraw.Draw(img, "RGBA")
    # badge: a white disc with a green arrow into a tray, bottom right, with a soft shadow
    cx, cy, r = S * 0.72, S * 0.72, S * 0.235
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((cx - r, cy - r + 14, cx + r, cy + r + 14), fill=(0, 0, 0, 150))
    img.alpha_composite(sh.filter(ImageFilter.GaussianBlur(16)))
    d = ImageDraw.Draw(img, "RGBA")
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(255, 255, 255, 255))
    d.ellipse((cx - r * 0.9, cy - r * 0.9, cx + r * 0.9, cy + r * 0.9), fill=(22, 150, 130, 255))
    w = (255, 255, 255, 255)
    shaft = r * 0.17
    d.rectangle((cx - shaft, cy - r * 0.58, cx + shaft, cy + r * 0.05), fill=w)
    d.polygon([(cx - r * 0.46, cy - r * 0.08), (cx + r * 0.46, cy - r * 0.08), (cx, cy + r * 0.42)], fill=w)
    tw = r * 0.58
    d.rounded_rectangle((cx - tw, cy + r * 0.5, cx + tw, cy + r * 0.66), radius=int(r * 0.07), fill=w)
    return img.convert("RGB")


def main():
    img = make()
    big = img.resize((512, 512), Image.LANCZOS)
    mask = Image.new("L", (512, 512), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 511, 511), radius=104, fill=255)
    out = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    out.paste(big, (0, 0), mask)
    out.save(os.path.join(HERE, "installer_512.png"))
    for dens, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(HERE, "setup_res", "mipmap-" + dens)
        os.makedirs(d, exist_ok=True)
        out.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
    print("installer icon written")


if __name__ == "__main__":
    main()
