#!/usr/bin/env python3
"""Draw the app icon (original artwork: a faceted crystal on a dark gradient) and write icon_512.png plus the launcher sizes in res/mipmap-*/.
  python3 make_icon.py          (needs Pillow)
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter

S = 1024
HERE = os.path.dirname(os.path.abspath(__file__))


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def make():
    img = Image.new("RGB", (S, S))
    px = img.load()
    top, bot = (18, 24, 64), (88, 52, 140)   # deep blue to violet
    for y in range(S):
        for x in range(S):
            t = min(1.0, (y / S) * 0.85 + (x / S) * 0.15)
            px[x, y] = lerp(top, bot, t)
    d = ImageDraw.Draw(img, "RGBA")
    # soft glow behind the crystal
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((S * 0.16, S * 0.16, S * 0.84, S * 0.84), fill=(150, 170, 255, 120))
    glow = glow.filter(ImageFilter.GaussianBlur(70))
    img.paste(glow, (0, 0), glow)
    d = ImageDraw.Draw(img, "RGBA")
    cx, cy = S / 2, S / 2
    top_p, bot_p = (cx, S * 0.13), (cx, S * 0.87)
    left_p, right_p = (S * 0.27, cy), (S * 0.73, cy)
    # crystal facets (a tall diamond split into lit / shaded faces)
    mid_t = (cx, S * 0.36)
    mid_b = (cx, S * 0.64)
    facets = [
        ([top_p, left_p, mid_t], (205, 228, 255, 255)),
        ([top_p, mid_t, right_p], (150, 190, 255, 255)),
        ([left_p, mid_t, mid_b], (120, 160, 240, 255)),
        ([mid_t, right_p, mid_b], (86, 120, 215, 255)),
        ([left_p, mid_b, bot_p], (70, 100, 200, 255)),
        ([mid_b, right_p, bot_p], (44, 66, 150, 255)),
    ]
    for pts, col in facets:
        d.polygon(pts, fill=col)
    # edges and a highlight line
    edge = (235, 245, 255, 230)
    for a, b in ((top_p, left_p), (top_p, right_p), (left_p, bot_p), (right_p, bot_p), (mid_t, mid_b), (left_p, right_p)):
        d.line([a, b], fill=edge, width=7)
    d.line([(S * 0.40, S * 0.30), (S * 0.46, S * 0.22)], fill=(255, 255, 255, 255), width=10)
    # small sparkles
    for (sx, sy, r) in ((0.80, 0.20, 34), (0.20, 0.78, 24), (0.84, 0.74, 18)):
        x, y, r = S * sx, S * sy, r
        d.polygon([(x, y - r), (x + r * 0.28, y - r * 0.28), (x + r, y), (x + r * 0.28, y + r * 0.28), (x, y + r),
                   (x - r * 0.28, y + r * 0.28), (x - r, y), (x - r * 0.28, y - r * 0.28)], fill=(255, 255, 255, 235))
    return img


def main():
    img = make()
    # rounded-square mask for the 512 store/desktop icon
    big = img.resize((512, 512), Image.LANCZOS)
    mask = Image.new("L", (512, 512), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 511, 511), radius=104, fill=255)
    out = Image.new("RGBA", (512, 512), (0, 0, 0, 0))
    out.paste(big, (0, 0), mask)
    out.save(os.path.join(HERE, "icon_512.png"))
    for dens, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        d = os.path.join(HERE, "res", "mipmap-" + dens)
        os.makedirs(d, exist_ok=True)
        small = out.resize((px, px), Image.LANCZOS)
        small.save(os.path.join(d, "ic_launcher.png"))
    print("icon written")


if __name__ == "__main__":
    main()
