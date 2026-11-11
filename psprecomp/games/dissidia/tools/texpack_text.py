#!/usr/bin/env python3
"""texpack_text.py - find tiny baked-in text in the originals with an OCR and redraw it crisp in the upscaled textures.

    texpack_text.py <extract dir> <pack dir> [--report DIR] [--only ID,ID] [--dry-run] [--min-corr 0.62]

Real-ESRGAN turns text a few pixels tall into mush. For every texture in <pack dir> whose original is in <extract dir>/src, this reads
the original (RapidOCR: pip install rapidocr-onnxruntime), keeps lines at most 10 px tall, and for each one:
  - finds the ink box, the ink colour and (when there is one) the dark drop shadow;
  - chooses the spacing of the recognised string by drawing it in Cinzel Bold at the original size and comparing with the original
    pixels; a line whose best drawing correlates below --min-corr is left alone (a misread, or a font Cinzel cannot imitate);
  - clears the model's blur inside the box and draws the line at the pack's scale (shadow first when there is one).
Writes the patched PNG over the pack file (not with --dry-run) and, with --report, a before/after picture per patched texture and a
log. texpack_patches.py (hand-made, exact) runs after this one and wins where both touch an atlas.
"""
import argparse
import itertools
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.path.join(HERE, "..", "steam", "fonts", "Cinzel-Bold.ttf")                       # capitals
FONT_MIXED = os.path.join(HERE, "..", "..", "..", "runtime", "assets", "fonts", "CrimsonText-SemiBold.ttf")   # lines with lower case
OCR_SCALE = 6
MAX_H = 10


_FONTS = {}


def render_mask(txt, w, h):
    mixed = any(c.islower() for c in txt)
    if mixed not in _FONTS:
        _FONTS[mixed] = ImageFont.truetype(FONT_MIXED if mixed else FONT, 200)
    f = _FONTS[mixed]
    bb = f.getbbox(txt)
    m = Image.new("L", (max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])), 0)
    ImageDraw.Draw(m).text((-bb[0], -bb[1]), txt, font=f, fill=255)
    return m.resize((max(1, w), max(1, h)), Image.LANCZOS)


def corr(a, b):
    a = a.astype(np.float32).ravel()
    b = b.astype(np.float32).ravel()
    a -= a.mean()
    b -= b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return float((a * b).sum() / d) if d > 1e-6 else 0.0


def ink_analysis(o, box):
    """coverage of the glyphs (0..1), their colour, the dark shadow colour or None, tight box"""
    x0, y0, x1, y1 = box
    reg = o[y0:y1 + 1, x0:x1 + 1].astype(np.float32)
    al = reg[..., 3] / 255.0
    lum = reg[..., :3].mean(-1)
    shadow = None
    if (al < 0.5).mean() > 0.25:                       # sprite on a transparent background: alpha is the coverage
        solid = al > 0.6
        if solid.sum() < 4:
            return None
        p95 = np.percentile(lum[solid], 95)
        if p95 > 200:                                  # light letters (white / pale glow around them): take the brightest pixels
            colour = reg[solid & (lum >= p95 - 12)][:, :3].mean(0)
        else:
            p = np.percentile(lum[solid], 85)
            colour = reg[solid & (lum >= p - 25)][:, :3].mean(0)
        dark = (al > 0.12) & (lum < colour.mean() - 60)
        cov = al * (~dark)
        if dark.sum() > 0.15 * (cov > 0.5).sum():
            shadow = reg[dark][:, :3].mean(0)
    else:                                              # text on an opaque panel: distance from the panel colour
        opaque = al > 0.6
        med = np.median(lum[opaque])
        d = np.abs(lum - med)
        if d.max() < 30:
            return None
        cov = np.clip(d / d.max(), 0, 1)
        cov[cov < 0.25] = 0
        top = cov > 0.7
        colour = reg[top][:, :3].mean(0) if top.sum() else reg[..., :3].reshape(-1, 3).mean(0)
    ys, xs = np.where(cov > 0.3)
    if len(xs) < 4:
        return None
    return dict(cov=cov, colour=colour, shadow=shadow, box=(x0 + xs.min(), y0 + ys.min(), x0 + xs.max(), y0 + ys.max()))


def best_spacing(txt, ink_mask_box, tight):
    """try the recognised text with 0..2 spaces inserted; return (text, corr)"""
    x0, y0, x1, y1 = tight
    w, h = x1 - x0 + 1, y1 - y0 + 1
    target = ink_mask_box
    letters = txt.replace(" ", "")
    best = (letters, -1.0)
    positions = range(1, len(letters))
    cands = [letters] + [letters[:i] + " " + letters[i:] for i in positions]
    for i, j in itertools.combinations(positions, 2):
        cands.append(letters[:i] + " " + letters[i:j] + " " + letters[j:])
    # corr at 4x the source size so thin strokes survive
    t = Image.fromarray((np.clip(target, 0, 1) * 255).astype(np.uint8)).resize((w * 4, h * 4), Image.BILINEAR).filter(ImageFilter.GaussianBlur(3))
    t = np.asarray(t).astype(np.float32)
    for c in cands:
        m = render_mask(c, w * 4, h * 4).filter(ImageFilter.GaussianBlur(3))
        r = corr(np.asarray(m).astype(np.float32), t)
        if r > best[1]:
            best = (c, r)
    return best


def draw_line(a, k, txt, tight, colour, shadow):
    x0, y0, x1, y1 = tight
    w, h = (x1 - x0 + 1) * k, (y1 - y0 + 1) * k
    m = render_mask(txt, w, h)
    H, W = a.shape[:2]
    cy0, cy1, cx0, cx1 = max(0, int((y0 - 0.7) * k)), min(H, int((y1 + 1.7) * k)), max(0, int((x0 - 0.7) * k)), min(W, int((x1 + 1.7) * k))
    base = a[cy0:cy1, cx0:cx1].astype(np.float32)
    base[..., 3] = 0
    canvas = Image.new("L", (W, H), 0)
    canvas.paste(m, (x0 * k, y0 * k))
    text = np.asarray(canvas).astype(np.float32)[cy0:cy1, cx0:cx1] / 255
    sh = np.zeros_like(text)
    if shadow is not None:
        c2 = Image.new("L", (W, H), 0)
        c2.paste(m, (x0 * k + k, y0 * k + k))
        sh = np.asarray(c2.filter(ImageFilter.GaussianBlur(k * 0.6))).astype(np.float32)[cy0:cy1, cx0:cx1] / 255 * 0.62
    col = np.array(colour, np.float32)
    shc = np.array(shadow if shadow is not None else colour, np.float32)
    a_out = text + sh * (1 - text)
    rgb = (col * text[..., None] + shc * (sh * (1 - text))[..., None]) / np.maximum(a_out[..., None], 1e-6)
    base[..., :3] = np.where(a_out[..., None] > 0, rgb, base[..., :3])
    base[..., 3] = a_out * 255
    a[cy0:cy1, cx0:cx1] = np.clip(base + 0.5, 0, 255).astype(np.uint8)


def split_words(letters, cov, box):
    """spaces from the ink gaps: gaps wider than ~half the cap height separate words; letters are shared out by segment width"""
    x0, y0, x1, y1 = box
    sub = cov[:, :] if cov.ndim == 2 else cov
    cols = (sub > 0.3).any(0)
    h = y1 - y0 + 1
    gaps, run = [], 0
    for x, c in enumerate(cols):
        if not c:
            run += 1
        else:
            if run >= max(2, int(round(h * 0.5))) and x - run > 0:
                gaps.append((x - run, x))
            run = 0
    if not gaps or len(letters) < 3:
        return letters
    edges = [0] + [g[0] for g in gaps] + [len(cols)]
    starts = [0] + [g[1] for g in gaps]
    widths = [e - s for s, e in zip(starts, edges[1:])]
    total = float(sum(widths))
    words, pos = [], 0
    for wd in widths[:-1]:
        n = max(1, int(round(len(letters) * wd / total)))
        words.append(letters[pos:pos + n]); pos += n
    words.append(letters[pos:])
    return " ".join(w for w in words if w)


def process(ocr, orig, hd, minc, log):
    k = hd.width // orig.width
    if k < 2 or hd.width != orig.width * k or hd.height != orig.height * k:
        return None, 0
    bg = Image.new("RGBA", orig.size, (110, 100, 150, 255))
    bg.alpha_composite(orig)
    big = np.asarray(bg.convert("RGB").resize((orig.width * OCR_SCALE, orig.height * OCR_SCALE), Image.BICUBIC))
    res, _ = ocr(big)
    o = np.array(orig)
    a = np.array(hd)
    n = 0
    for box, txt, conf in res or []:
        xs = [p[0] / OCR_SCALE for p in box]
        ys = [p[1] / OCR_SCALE for p in box]
        bx0, bx1 = int(max(0, min(xs) - 1)), int(min(orig.width - 1, max(xs) + 1))
        by0, by1 = int(max(0, min(ys) - 1)), int(min(orig.height - 1, max(ys) + 1))
        letters = "".join(c for c in txt if c.isalnum() or c in "'!?&-:./+")
        if float(conf) < 0.6 or len(letters) < 2 or sum(c.isalpha() for c in letters) < 2 or by1 - by0 + 1 > MAX_H + 3:
            log.append("skip %r conf %.2f h=%d" % (txt, float(conf), by1 - by0 + 1))
            continue
        an = ink_analysis(o, (bx0, by0, bx1, by1))
        if an is None:
            continue
        tx0, ty0, tx1, ty1 = an["box"]
        if ty1 - ty0 + 1 > MAX_H:
            log.append("skip %r tall" % txt)
            continue
        mask = an["cov"][ty0 - by0:ty1 - by0 + 1, tx0 - bx0:tx1 - bx0 + 1].astype(np.float32)
        s, r = best_spacing(letters, mask, an["box"])
        if r < minc:
            log.append("reject %r -> %r corr %.2f" % (txt, s, r))
            continue
        draw_line(a, k, s, an["box"], an["colour"], an["shadow"])
        log.append("ok %r corr %.2f%s" % (s, r, " +shadow" if an["shadow"] is not None else ""))
        n += 1
    return (Image.fromarray(a, "RGBA") if n else None), n


def scan(ocr, orig):
    """OCR proposals for one original: [{text, conf, box (ink, source px), colour, shadow}] for lines <= MAX_H px tall"""
    bg = Image.new("RGBA", orig.size, (110, 100, 150, 255))
    bg.alpha_composite(orig)
    big = np.asarray(bg.convert("RGB").resize((orig.width * OCR_SCALE, orig.height * OCR_SCALE), Image.BICUBIC))
    res, _ = ocr(big)
    o = np.array(orig)
    out = []
    for box, txt, conf in res or []:
        xs = [p[0] / OCR_SCALE for p in box]
        ys = [p[1] / OCR_SCALE for p in box]
        bx0, bx1 = int(max(0, min(xs) - 1)), int(min(orig.width - 1, max(xs) + 1))
        by0, by1 = int(max(0, min(ys) - 1)), int(min(orig.height - 1, max(ys) + 1))
        letters = "".join(c for c in txt if c.isalnum() or c in "'!?&-:./+%")
        if float(conf) < 0.55 or len(letters) < 2 or sum(c.isalpha() for c in letters) < 2:
            continue
        an = ink_analysis(o, (bx0, by0, bx1, by1))
        if an is None:
            continue
        tx0, ty0, tx1, ty1 = an["box"]
        if ty1 - ty0 + 1 > MAX_H or ty1 - ty0 + 1 < 4:
            continue
        cov = an["cov"][ty0 - by0:ty1 - by0 + 1, tx0 - bx0:tx1 - bx0 + 1]
        out.append(dict(text=split_words(letters, cov, an["box"]), ocr=txt, conf=round(float(conf), 2), box=[int(v) for v in an["box"]],
                        colour=[int(v) for v in an["colour"]], shadow=None if an["shadow"] is None else [int(v) for v in an["shadow"]]))
    return out


def stretch(txt, box):
    """how much the font has to be stretched to fill the original's box (1.0 = natural proportions)"""
    mixed = any(c.islower() for c in txt)
    if mixed not in _FONTS:
        _FONTS[mixed] = ImageFont.truetype(FONT_MIXED if mixed else FONT, 200)
    bb = _FONTS[mixed].getbbox(txt)
    nat = (bb[2] - bb[0]) / max(1, bb[3] - bb[1])
    return ((box[2] - box[0] + 1) / max(1, box[3] - box[1] + 1)) / nat


def apply_proposals(orig, hd, lines, max_stretch=1.25):
    k = hd.width // orig.width
    a = np.array(hd)
    for ln in lines:
        if ln.get("skip"):
            continue
        st = stretch(ln["text"], ln["box"])
        if not (1 / max_stretch <= st <= max_stretch):
            ln["skip_reason"] = "stretch %.2f" % st
            continue
        draw_line(a, k, ln["text"], tuple(ln["box"]), ln["colour"], ln["shadow"])
    return Image.fromarray(a, "RGBA")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ext")
    ap.add_argument("pack")
    ap.add_argument("--report")
    ap.add_argument("--only")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--min-corr", type=float, default=0.62)
    ap.add_argument("--max-size", type=int, default=512, help="skip originals larger than this (model textures)")
    ap.add_argument("--scan", metavar="JSON", help="write OCR proposals for every texture to JSON (edit it, then --apply)")
    ap.add_argument("--apply", metavar="JSON", help="draw the proposals of a reviewed JSON (lines with \"skip\": true are left alone)")
    a = ap.parse_args()
    if a.apply:
        props = json.load(open(a.apply))
        n = 0
        for i, lines in props.items():
            src, dst = os.path.join(a.ext, "src", i + ".png"), os.path.join(a.pack, i + ".png")
            if not (os.path.exists(src) and os.path.exists(dst)) or not any(not l.get("skip") for l in lines):
                continue
            Image.open(dst).convert("RGBA")  # must exist
            out = apply_proposals(Image.open(src).convert("RGBA"), Image.open(dst).convert("RGBA"), lines)
            if not a.dry_run:
                out.save(dst, optimize=True)
            n += sum(1 for l in lines if not l.get("skip"))
        print("lines drawn:", n)
        return
    if a.scan:
        from rapidocr_onnxruntime import RapidOCR
        ocr = RapidOCR()
        ids = a.only.split(",") if a.only else sorted(f[:-4] for f in os.listdir(a.pack) if f.endswith(".png"))
        props = {}
        for k, i in enumerate(ids):
            src = os.path.join(a.ext, "src", i + ".png")
            if not os.path.exists(src):
                continue
            orig = Image.open(src).convert("RGBA")
            if max(orig.size) > a.max_size:
                continue
            ls = scan(ocr, orig)
            if ls:
                props[i] = ls
            if k % 100 == 0:
                print("[scan] %d/%d, %d textures with text" % (k, len(ids), len(props)), file=sys.stderr, flush=True)
        json.dump(props, open(a.scan, "w"), indent=0)
        print("textures with small text:", len(props), "lines:", sum(len(v) for v in props.values()))
        return
    from rapidocr_onnxruntime import RapidOCR
    ocr = RapidOCR()
    ids = a.only.split(",") if a.only else sorted(f[:-4] for f in os.listdir(a.pack) if f.endswith(".png"))
    if a.report:
        os.makedirs(a.report, exist_ok=True)
    total = 0
    for k, i in enumerate(ids):
        src = os.path.join(a.ext, "src", i + ".png")
        dst = os.path.join(a.pack, i + ".png")
        if not (os.path.exists(src) and os.path.exists(dst)):
            continue
        orig = Image.open(src).convert("RGBA")
        if max(orig.size) > a.max_size:
            continue
        hd = Image.open(dst).convert("RGBA")
        log = []
        out, n = process(ocr, orig, hd, a.min_corr, log)
        if n:
            total += n
            print("%s %dx%d: %d line(s) %s" % (i, orig.width, orig.height, n, "; ".join(l for l in log if l.startswith("ok"))[:200]), flush=True)
            if a.report:
                json.dump(log, open(os.path.join(a.report, i + ".log.json"), "w"))
                bgc = lambda x: (lambda c: (c.alpha_composite(x), c)[1])(Image.new("RGBA", x.size, (110, 100, 150, 255))).convert("RGB")
                kk = hd.width // orig.width
                pair = Image.new("RGB", (hd.width * 2 + 10, hd.height), (0, 0, 0))
                pair.paste(bgc(hd), (0, 0))
                pair.paste(bgc(out), (hd.width + 10, 0))
                pair.save(os.path.join(a.report, i + ".png"))
            if not a.dry_run:
                out.save(dst, optimize=True)
        if k % 100 == 0:
            print("[text] %d/%d, %d lines" % (k, len(ids), total), file=sys.stderr, flush=True)
    print("lines redrawn:", total)


if __name__ == "__main__":
    main()
