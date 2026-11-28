#!/usr/bin/env python3
"""hot_funcs.py WORK fc.1 fc.2 OUT_TXT [--cover 0.97] (writes tools/hot_funcs.txt): which generated functions are hot. weight = entry count * function length (lines).
Menu-phase counts (fc.1: up to the battle start) belong to the main code + menu/extra/...; battle counts (fc.2 - fc.1) to main + battle overlay."""
import sys, os, re, glob, json
work, f1, f2, out = sys.argv[1:5]
cover = float(sys.argv[sys.argv.index("--cover") + 1]) if "--cover" in sys.argv else 0.97
def rd(p):
    d = {}
    for l in open(p):
        a, c = l.split(); d[int(a, 16)] = int(c)
    return d
c1, c2 = rd(f1), rd(f2)
battle = {a: c - c1.get(a, 0) for a, c in c2.items() if c - c1.get(a, 0) > 0}
menu = c1
groups = {"out": work + "/output/generated"}
for g in ("battle", "menu", "extra", "supacc", "xmods"):
    groups[g] = work + "/overlays/%s/generated" % g
pat = re.compile(r"^RECOMP_FUNC void \w+\(.*\{ /\* 0x([0-9A-Fa-f]{8}) \*/")
funcs = []   # (group, file, addr, lines)
for g, d in groups.items():
    for f in sorted(glob.glob(d + "/batch_*.cpp")):
        cur = None; n = 0
        for line in open(f, errors="replace"):
            m = pat.match(line)
            if m:
                if cur: funcs.append((g, os.path.basename(f), cur, n))
                cur = int(m.group(1), 16); n = 0
            n += 1
        if cur: funcs.append((g, os.path.basename(f), cur, n))
print("functions", len(funcs))
w = {}   # (group, addr) -> weight
tot = 0
for g, fn, a, n in funcs:
    cnt = (battle.get(a, 0) if g in ("out", "battle") else 0) + (menu.get(a, 0) if g in ("out", "menu", "extra", "supacc", "xmods") else 0)
    if cnt:
        w[(g, a)] = cnt * n; tot += cnt * n
srt = sorted(w.items(), key=lambda kv: -kv[1])
acc = 0; hot = []
for k, v in srt:
    hot.append(k); acc += v
    if acc >= cover * tot: break
print("executed funcs", len(w), "hot funcs", len(hot), "cover", cover)
hs = set(hot)
files = {}
for g, fn, a, n in funcs:
    files.setdefault((g, fn), [0, 0, 0]); files[(g, fn)][0] += 1
    if (g, a) in hs: files[(g, fn)][1] += 1
    files[(g, fn)][2] += w.get((g, a), 0)
hotfiles = [k for k, v in files.items() if v[1]]
print("files", len(files), "files with hot funcs", len(hotfiles))
with open(out, "w") as o:
    o.write("# functions that run a lot (battle + menus), by entry count x length: %d%% of the executed code. group addr\n" % round(cover * 100))
    o.write("# a batch file containing any of them is compiled with -O2 in the phone's Fast build. Made by tools/hot_funcs.py\n")
    for (g, a) in sorted(hs): o.write("%s %08X\n" % (g, a))
json.dump({"%s/%s" % k: v for k, v in files.items()}, open(out + ".files.json", "w"))
