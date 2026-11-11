#!/usr/bin/env python3
"""texpack_extract.py - every GIM texture on the disc, offline (no game run): PNGs named by texture-pack id.

The runtime names a replacement <pack>/<id>.png after a hash of what the GE decodes (texpack_id.py); a texture dump
(PSPRECOMP_TEX_DUMP=1) only holds what the player has seen on screen. This reads the same ids straight from the
game's PACKAGE.BIN instead: every `MIG.00.1PSP` GIM anywhere in an entry (stand-alone GIMs, models' texture images,
menu paks, objx) is decoded the way the game uploads it, so one run covers the whole game, DLC costumes and
unused art included.

    texpack_extract.py <install dir | DATA dir> --out DIR [--pack PPSSPP_PACK_DIR] [--min-size N]

Writes DIR/src/<id>.png (the original RGBA) and DIR/index.tsv:

    id  WxH  fmt  uses  covered  first-source [more sources]

`covered` is 1 when the PPSSPP-style pack given with --pack (textures.ini + PNG) already replaces the texture. That
test uses the content half of PPSSPP's key (xxh64 of the raw bytes in guest memory, which is the GIM's pixel data as it
is stored); the palette half depends on guest memory beside the palette, so it cannot be computed here. A texture
whose pixels are in the pack under another palette counts as covered: a palette variant the pack does not have
keeps its original look, which is the safe direction.

Only the 8-bit and 4-bit indexed and 32-bit GIMs the game uses are handled. Requires numpy, Pillow, xxhash (pip install
xxhash; without it a pure Python xxh64 is used, about 20x slower).
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
if __name__ == "__main__":
    import pydeps
    pydeps.require()
import numpy as np                      # noqa: E402
from PIL import Image                   # noqa: E402
import charmod_gim as G                 # noqa: E402
import package_extract as pe            # noqa: E402
import texpack_id as ti                 # noqa: E402

try:
    import xxhash
    def xxh64(b, seed=0):
        return xxhash.xxh64_intdigest(b, seed)
except ImportError:                     # pure Python fallback (texpack_id.py)
    xxh64 = ti.xxh64

MAGIC = b"MIG.00.1PSP"
GE_CLUT4, GE_CLUT8, GE_8888 = 4, 5, 3
SKIP_EXT = {"at3", "scd", "pmf", "evex"}


def gim_blobs(d):
    """Every GIM inside d: yields the bytes of each."""
    o = 0
    while True:
        o = d.find(MAGIC, o)
        if o < 0:
            return
        if o + 24 <= len(d):
            _, _, size = struct.unpack_from("<HHI", d, o + 16)
            if 0 < size <= len(d) - o - 16:
                yield d[o:o + 16 + size]
        o += 16


def palette_rgba(g, praw, pfmt, n):
    """GE-style RGBA of the first n palette entries (the runtime's read_clut_palette), padded to 256."""
    out = np.zeros((256, 4), np.uint8)
    if pfmt == 3:
        a = np.frombuffer(praw[:4 * n], np.uint8).reshape(-1, 4)
        out[:len(a)] = a
    else:
        v = np.frombuffer(praw[:2 * n], "<u2").astype(np.uint32)
        if pfmt == 0:
            c = [(v & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, ((v >> 11) & 31) * 255 // 31, np.full_like(v, 255)]
        elif pfmt == 1:
            c = [(v & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, ((v >> 10) & 31) * 255 // 31, (v >> 15) * 255]
        else:
            c = [(v & 15) * 255 // 15, ((v >> 4) & 15) * 255 // 15, ((v >> 8) & 15) * 255 // 15, ((v >> 12) & 15) * 255 // 15]
        out[:len(v)] = np.stack(c, 1).astype(np.uint8)
    return out


def ppsspp_content_hash(raw, bpp, w, h, bufw):
    """The runtime's psp_texpack_ppsspp_content_hash (xxh64 kind), over the stored (swizzled) bytes."""
    def hsh(b):
        return xxh64(bytes(b), 0xBACD7814) & 0xFFFFFFFF
    if bufw <= w:
        total = bufw * h + (w - bufw)
        return hsh(raw[:bpp * total // 8])
    per, stride = bpp * w // 8, bpp * bufw // 8
    r = 0
    for y in range(h):
        r = ((r * 11) & 0xFFFFFFFF) ^ hsh(raw[y * stride:y * stride + per])
    return r


def decode_gim(g):
    """-> dict(w, h, fmt, rgba (H,W,4), id, chash) or None for a GIM this tool does not handle."""
    ims, pals = G._find(g, 4), G._find(g, 5)
    if len(ims) != 1 or len(pals) > 1:
        return None
    im = G._read_image_block(g, ims[0])
    w, h, gf, order = im["width"], im["height"], im["format"], im["order"]
    pitch, rows, raw = im["pitch"], im["rows"], im["raw"]
    if len(raw) < pitch * rows or w < 1 or h < 1:
        return None
    bpp = G._BPP.get(gf)
    if bpp is None:
        return None
    stored = raw[:pitch * rows]
    lin = G._unswizzle(stored, pitch, rows) if order == 1 else stored
    px = np.frombuffer(lin, np.uint8).reshape(rows, pitch)[:h]
    bufw = pitch * 8 // bpp
    chash = ppsspp_content_hash(stored, bpp, w, h, bufw)
    if gf in (4, 5):
        if len(pals) != 1:
            return None
        pb = G._read_image_block(g, pals[0])
        pal = palette_rgba(g, pb["raw"], pb["format"], pb["width"])
        if gf == 5:
            texels = np.ascontiguousarray(px[:, :w])
            idx = texels
            fmt = GE_CLUT8
        else:
            texels = np.ascontiguousarray(px[:, :(w + 1) // 2])
            idx = np.stack([texels & 15, texels >> 4], 2).reshape(h, -1)[:, :w]
            fmt = GE_CLUT4
        tb = texels.tobytes()
        used = np.unique(texels if gf == 5 else np.concatenate([(texels & 15).ravel(), (texels >> 4).ravel()]))
        buf = bytearray()
        for i in used:
            buf.append(int(i))
            buf += pal[i].tobytes()
        clut_h = xxh64(bytes(buf), 1)
        rgba = pal[idx]
    elif gf == 3:
        texels = np.ascontiguousarray(px[:, :w * 4])
        tb = texels.tobytes()
        clut_h = 0
        fmt = GE_8888
        rgba = texels.reshape(h, w, 4)
    else:
        return None
    raw_h = xxh64(tb, 0)
    tid = xxh64(struct.pack("<5Q", raw_h, clut_h, fmt, w, h), 0x70737074)
    return dict(w=w, h=h, fmt=fmt, rgba=rgba, id=tid, chash=chash)


def pack_content_hashes(pack_dir):
    """The content halves (last 8 hex digits) of every [hashes] key whose PNG exists."""
    out = set()
    ini = os.path.join(pack_dir, "textures.ini")
    sec = ""
    for line in open(ini, errors="replace"):
        s = line.strip()
        if s.startswith("["):
            sec = s.lower()
            continue
        if sec != "[hashes]" or "=" not in s or s[0] in "#;":
            continue
        k, v = (x.strip() for x in s.split("=", 1))
        if len(k) != 24 or not v.lower().endswith(".png"):
            continue
        if os.path.exists(os.path.join(pack_dir, v.replace("\\", "/"))):
            out.add(int(k[16:], 16))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data", help="install folder (with disc0/) or the DATA folder holding PACKAGE.BIN")
    ap.add_argument("--out", required=True)
    ap.add_argument("--pack", help="PPSSPP-style pack folder (textures.ini) whose textures count as covered")
    ap.add_argument("--min-size", type=int, default=0, help="skip textures smaller than this in both directions")
    ap.add_argument("--check-id", metavar="GIMFILE", help="print the id of a GIM file (compare with texpack_id.py) and exit")
    a = ap.parse_args()
    if a.check_id:
        r = decode_gim(open(a.check_id, "rb").read())
        print("%016x %dx%d" % (r["id"], r["w"], r["h"]) if r else "unsupported")
        return
    pk = pe.Package(a.data)
    covered_h = pack_content_hashes(a.pack) if a.pack else set()
    names = {}
    pj = os.path.join(HERE, "..", "package_paths.txt")
    if os.path.exists(pj):
        for line in open(pj):
            p = line.strip()
            names[pe.package_crc(p)] = p
    src_dir = os.path.join(a.out, "src")
    os.makedirs(src_dir, exist_ok=True)
    seen = {}                       # id -> [w,h,fmt,uses,covered,[sources]]
    n_gim = n_skip = 0
    entries = sorted(pk.entries)
    for k, crc in enumerate(entries):
        name = names.get(crc, "%08x" % crc)
        if name.rsplit(".", 1)[-1].lower() in SKIP_EXT:
            continue
        d = pk.read_crc(crc)
        for gi, g in enumerate(gim_blobs(d)):
            n_gim += 1
            r = decode_gim(g)
            if r is None:
                n_skip += 1
                continue
            if r["w"] < a.min_size and r["h"] < a.min_size:
                continue
            e = seen.get(r["id"])
            if e is None:
                Image.fromarray(r["rgba"], "RGBA").save(os.path.join(src_dir, "%016x.png" % r["id"]))
                seen[r["id"]] = e = [r["w"], r["h"], r["fmt"], 0, int(r["chash"] in covered_h), []]
            e[3] += 1
            if len(e[5]) < 3:
                e[5].append("%s#%d" % (name, gi))
        if k % 1000 == 0:
            print("[extract] %d/%d entries, %d GIMs, %d unique" % (k, len(entries), n_gim, len(seen)), file=sys.stderr, flush=True)
    names_fmt = {GE_CLUT4: "CLUT4", GE_CLUT8: "CLUT8", GE_8888: "8888"}
    with open(os.path.join(a.out, "index.tsv"), "w") as f:
        f.write("# id\tWxH\tfmt\tuses\tcovered\tsources\n")
        for i, (w, h, fmt, uses, cov, srcs) in sorted(seen.items()):
            f.write("%016x\t%dx%d\t%s\t%d\t%d\t%s\n" % (i, w, h, names_fmt[fmt], uses, cov, " ".join(srcs)))
    cov = sum(1 for v in seen.values() if v[4])
    print("[extract] %d GIMs (%d unsupported), %d unique textures, %d covered by the pack, %d uncovered -> %s"
          % (n_gim, n_skip, len(seen), cov, len(seen) - cov, a.out))


if __name__ == "__main__":
    main()
