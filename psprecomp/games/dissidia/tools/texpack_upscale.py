#!/usr/bin/env python3
"""texpack_upscale.py - batch-upscale texpack_extract.py output with realesrgan-ncnn-vulkan, with the safety rules we settled on.

    texpack_upscale.py <extract dir> <out dir> --esrgan DIR_WITH_BINARY [--limit N]

Reads <extract>/index.tsv, upscales every uncovered texture (covered=0) into <out>/<id>.png:
  - map/stage textures (sources under map/ ) use realesrgan-x4plus, everything else realesrgan-x4plus-anime;
  - near-uniform textures (colour+alpha std < 3) get a plain Lanczos 4x: a model would invent detail on flat art;
  - the original alpha channel is resized (Lanczos) and re-attached, so sprite edges stay exact;
  - textures whose larger side is > 256 are brought back down to 2x (repo size), the rest stay 4x.
Finished ids are skipped, so a run can be resumed. Needs numpy + Pillow.
"""
import argparse, os, shutil, subprocess, sys
import numpy as np
from PIL import Image

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ext'); ap.add_argument('out'); ap.add_argument('--esrgan', required=True)
    ap.add_argument('--limit', type=int, default=0)
    a = ap.parse_args()
    rows = []
    for l in open(os.path.join(a.ext, 'index.tsv')):
        if l.startswith('#'): continue
        f = l.rstrip('\n').split('\t')
        if f[4] == '0': rows.append((f[0], f[5]))
    if a.limit: rows = rows[:a.limit]
    a.out = os.path.abspath(a.out); a.ext = os.path.abspath(a.ext); a.esrgan = os.path.abspath(a.esrgan)
    os.makedirs(a.out, exist_ok=True)
    work = os.path.join(a.out, '.work'); shutil.rmtree(work, ignore_errors=True)
    todo = {'x4plus': [], 'x4plus-anime': []}
    flat = []
    for i, src in rows:
        if os.path.exists(os.path.join(a.out, i + '.png')): continue
        im = np.array(Image.open(os.path.join(a.ext, 'src', i + '.png')).convert('RGBA')).astype(float)
        if im.std(axis=(0, 1)).max() < 3: flat.append(i); continue
        todo['x4plus' if ' ' not in src and src.startswith('map/') or src.split('#')[0].startswith('map/') else 'x4plus-anime'].append(i)
    print('flat', len(flat), {k: len(v) for k, v in todo.items()}, flush=True)
    raw = {}
    for model, ids in todo.items():
        if not ids: continue
        d_in = os.path.join(work, model, 'in'); d_out = os.path.join(work, model, 'out')
        os.makedirs(d_in); os.makedirs(d_out)
        for i in ids: shutil.copy(os.path.join(a.ext, 'src', i + '.png'), os.path.join(d_in, i + '.png'))
        r = subprocess.run([os.path.join(a.esrgan, 'realesrgan-ncnn-vulkan'), '-i', d_in, '-o', d_out, '-n', 'realesrgan-' + model, '-s', '4', '-g', '0'],
                           cwd=a.esrgan, stderr=None)
        for i in ids: raw[i] = os.path.join(d_out, i + '.png')
    def finish(i, up):
        o = Image.open(os.path.join(a.ext, 'src', i + '.png')).convert('RGBA')
        w, h = o.size
        s = 4 if max(w, h) <= 256 else 2
        tgt = (w * s, h * s)
        if up is None: u = o.resize(tgt, Image.LANCZOS)
        else:
            u = Image.open(up).convert('RGBA')
            if u.size != tgt: u = u.resize(tgt, Image.LANCZOS)
            if o.getchannel('A').getextrema() != (255, 255): u.putalpha(o.getchannel('A').resize(tgt, Image.LANCZOS))
            else: u.putalpha(255)
        u.save(os.path.join(a.out, i + '.png'), optimize=True)
    n = 0
    for i in flat: finish(i, None); n += 1
    for i, p in raw.items():
        if os.path.exists(p): finish(i, p); n += 1
    shutil.rmtree(work, ignore_errors=True)
    print('wrote', n, 'of', len(rows), '->', a.out)

if __name__ == '__main__': main()
