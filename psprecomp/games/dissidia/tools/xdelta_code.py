#!/usr/bin/env python3
"""`xdelta_mod.py build-code`: recompile the code that installed xdelta mods change (see xdelta_mod.py).

For every mod folder with "kind": "xdelta" and a code/ folder, and every module it patches (EBOOT, battle / menu /
extra overlay):

1. the module's analysis (<work>/analysis*.json, the retail one) gets the patched bytes; an overlay that grew (code
   appended after its end, a common trick of ISO patches) gets a longer segment,
2. jump targets the patched code needs that the retail analysis does not have become force_entries (a call target or
   a jump into code no function covers) and force_mid_entries (a jump into the middle of a function),
3. the module is recompiled twice the way the pipeline does it (build_overlay.py, PSPRECOMP_CROSS_MID=1), from the
   retail and from the patched analysis, under one symbol prefix (a few seconds each), and only the functions whose
   C++ differs are kept, plus the mid-entry wrappers of those functions.

Everything goes into one function set, <work>/overlays/xmods (generated/batch_*.cpp, funcs.h, mid_entries.cpp and
dispatch.cpp with the table `dissidia_xmod_sets()` that games/dissidia/runtime/xdelta_mods.cpp reads). The set always
exists (empty without mods) because the runtime links against it. Files are only rewritten when their content changes,
so an unchanged set does not recompile.
"""
import base64
import bisect
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discdir  # noqa: E402  (where disc0 is)
import xdelta_mod as X  # noqa: E402

FUNC_RE = re.compile(r'^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{ /\* 0x([0-9A-Fa-f]{8}) \*/', re.M)
MID_RE = re.compile(r'RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{\n    ctx->entry_point = 0x([0-9A-F]{8})U;\n'
                    r'    (\w+)\(rdram, ctx\);\n    ctx->entry_point = 0;\n\}\n')
SYM_RE = re.compile(r'\b(?:[A-Za-z0-9]+_)*?((?:FUN|func)_[0-9A-Fa-f]{8}\w*)')


def say(msg):
    print(msg, flush=True)


def functions_of(gen_dir):
    """{addr: (name, text)} of every function in the batch files of a recompiled set."""
    out = {}
    if not os.path.isdir(gen_dir):
        return out
    for fn in sorted(os.listdir(gen_dir)):
        if not (fn.startswith("batch_") and fn.endswith(".cpp")):
            continue
        s = open(os.path.join(gen_dir, fn)).read()
        ms = list(FUNC_RE.finditer(s))
        for i, m in enumerate(ms):
            end = ms[i + 1].start() if i + 1 < len(ms) else len(s)
            body = s[m.start():end].rstrip() + "\n"
            out[int(m.group(2), 16)] = (m.group(1), body)
    return out


def decode_jumps(seg_va, data, lo, hi):
    """[(at, kind, target)] for j / jal in [lo, hi) of a segment."""
    out = []
    for va in range(lo & ~3, hi, 4):
        o = va - seg_va
        if o < 0 or o + 4 > len(data):
            continue
        w = struct.unpack_from("<I", data, o)[0]
        op = w >> 26
        if op in (2, 3):
            t = ((va + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            out.append((va, "jal" if op == 3 else "j", t))
    return out


def patched_segments(module, analysis, blob):
    """New analysis segments (list of dicts) with the patched bytes."""
    segs = []
    if module == 0:
        phs = X.elf_segments(blob)
        for s in analysis["segments"]:
            va = int(s["p_vaddr"], 16)
            old = base64.b64decode(s["data_b64"])
            ph = [p for p in phs if p[1] == va]
            if not ph:
                raise ValueError("patched EBOOT has no segment at 0x%08X" % va)
            off, _, fs, ms = ph[0]
            data = blob[off:off + fs].ljust(len(old), b"\0")[:max(len(old), fs)]
            n = dict(s)
            n["data_b64"] = base64.b64encode(data).decode()
            segs.append(n)
    else:
        if len(analysis["segments"]) != 1:
            raise ValueError("overlay analysis with %d segments" % len(analysis["segments"]))
        s = analysis["segments"][0]
        old = base64.b64decode(s["data_b64"])
        _, va, fs, ms = X.elf_segments(blob)[0]
        body = blob[X.OVL_FILE_HDR:X.OVL_FILE_HDR + fs]       # filesz may exceed memsz: code appended in a patch
        data = body.ljust(len(old), b"\0")
        n = dict(s)
        n["data_b64"] = base64.b64encode(data).decode()
        n["p_filesz"] = len(data) if isinstance(s.get("p_filesz"), int) else s.get("p_filesz")
        n["p_memsz"] = len(data) if isinstance(s.get("p_memsz"), int) else s.get("p_memsz")
        segs.append(n)
    return segs


def changed_ranges(old, new, va):
    out, i, n = [], 0, max(len(old), len(new))
    old, new = old.ljust(n, b"\0"), new.ljust(n, b"\0")
    while i < n:
        if old[i] != new[i]:
            j = i
            while j < n and old[j] != new[j]:
                j += 1
            out.append((va + i, va + j))
            i = j
        else:
            i += 1
    return out


def entries_for(analysis, segs_old, segs_new):
    """force_entries / force_mid_entries the patched code needs."""
    funcs = sorted((int(f["address"], 16), int(f["size"])) for f in analysis["functions"] if f.get("size"))
    starts = {a for a, _ in funcs}
    mids = {int(m.get("addr", "0"), 16) for m in analysis.get("mid_entries", []) if m.get("addr")}

    def owner(t):
        best = None
        for a, sz in funcs:
            if a < t < a + sz:
                best = (a, sz) if best is None or a > best[0] else best
        return best

    force, mid = set(), {}
    for so, sn in zip(segs_old, segs_new):
        va = int(sn["p_vaddr"], 16)
        old, new = base64.b64decode(so["data_b64"]), base64.b64decode(sn["data_b64"])
        for lo, hi in changed_ranges(old, new, va):
            # the whole words around a change, and everything appended after the retail end
            for at, kind, t in decode_jumps(va, new, lo - 4, hi + 4):
                if t in starts or t in mids or not (va <= t < va + len(new)):
                    continue
                o = owner(t)
                if kind == "j" and o:
                    mid[t] = o[0]
                else:
                    force.add(t)
        if len(new) > len(old):                       # code past the retail end: jumps inside it too
            for at, kind, t in decode_jumps(va, new, va + len(old), va + len(new)):
                if t in starts or t in mids or not (va <= t < va + len(new)):
                    continue
                o = owner(t)
                if kind == "j" and o:
                    mid[t] = o[0]
                else:
                    force.add(t)
    for t in list(mid):
        if t in force:
            del mid[t]
    return sorted(force), sorted(mid.items())


def new_code_ranges(segs_old, segs_new):
    """[(lo, hi)] vaddr ranges the patch wrote: changed bytes, and everything past the retail end of a segment."""
    out = []
    for so, sn in zip(segs_old, segs_new):
        va = int(sn["p_vaddr"], 16)
        old, new = base64.b64decode(so["data_b64"]), base64.b64decode(sn["data_b64"])
        out += changed_ranges(old[:len(new)], new[:len(old)], va)
        if len(new) > len(old):
            out.append((va + len(old), va + len(new)))
    return out


def pointer_targets(segs_new, ranges):
    """Words of the module that point into code the patch wrote: function-pointer tables, the patch's own or the
    game's (a patch may overwrite a function the analysis missed and that only a table points to). The code reaches
    them with jalr / jr, which no jump decoding sees."""
    starts = sorted(lo for lo, hi in ranges)
    out = set()
    for sn in segs_new:
        data = base64.b64decode(sn["data_b64"])
        n = len(data) // 4
        for v in struct.unpack_from("<%dI" % n, data, 0):
            if v & 3:
                continue
            i = bisect.bisect_right(starts, v) - 1
            if i >= 0:
                lo = starts[i]
                hi = max(h for l, h in ranges if l == lo)
                if lo <= v < hi:
                    out.add(v)
    return out


def missing_targets(set_dir):
    """Static RECOMP_LOOKUP targets of a recompiled set that are not in its dispatch table (recompile_report.json)."""
    try:
        r = json.load(open(os.path.join(set_dir, "recompile_report.json")))
        return {int(x, 16) for x in r.get("dispatch_audit", {}).get("missing_targets", [])}
    except (OSError, ValueError):
        return set()


def toml_with(base_toml, force, mid):
    s = open(base_toml).read() if base_toml and os.path.isfile(base_toml) else "[recompile]\n"
    if force:
        extra = ", ".join('"0x%08X"' % a for a in force)
        m = re.search(r'^force_entries\s*=\s*\[', s, re.M)
        if m:
            s = s[:m.end()] + extra + ", " + s[m.end():]
        else:
            m = re.search(r'^\[recompile\]\s*$', s, re.M)
            if m:
                s = s[:m.end()] + "\nforce_entries = [%s]" % extra + s[m.end():]
            else:
                s = "[recompile]\nforce_entries = [%s]\n" % extra + s
    for e, p in mid:
        s += '\n[[recompile.force_mid_entries]]\nentry  = "0x%08X"\nparent = "0x%08X"\n' % (e, p)
    return s


def c_ident(s):
    s = re.sub(r"[^A-Za-z0-9]", "_", s)
    return ("m" + s) if not s or s[0].isdigit() else s


class Ctx:
    def __init__(self, work, game, env):
        self.work, self.game, self.env = work, game, env

    def recompile(self, module, prefix, analysis_path, toml, od, what):
        r = subprocess.run(([os.environ["PSPRECOMP_PY_LOADER"]] if os.environ.get("PSPRECOMP_PY_LOADER") else []) + [sys.executable, os.path.join(HERE, "build_overlay.py"), str(module), prefix,
                            analysis_path, os.path.join(self.work, "analysis.json"), od, toml],
                           env=self.env, capture_output=True, text=True)
        if r.returncode != 0:
            say("  %s: recompile failed:\n%s%s" % (what, r.stdout[-2000:], r.stderr[-2000:]))
            raise SystemExit(1)
        return functions_of(os.path.join(od, "generated"))


def kept_mids(od, kept_names):
    mtext = open(os.path.join(od, "mid_entries.cpp")).read()
    return [(int(m.group(2), 16), m.group(1), m.group(0)) for m in MID_RE.finditer(mtext) if m.group(3) in kept_names]


LABEL_RE = re.compile(r'^L_([0-9A-F]{8}):', re.M)


def compiled_extents(gen_dir):
    """[(start, size)] of every function in a recompiled set: from its address to its last instruction label."""
    out = []
    for addr, (_n, body) in functions_of(gen_dir).items():
        labels = [int(x, 16) for x in LABEL_RE.findall(body)]
        end = max([addr] + labels) + 4
        out.append((addr, end - addr))
    return sorted(out)


def compiled_mids(path):
    """[(entry, parent start)] of the mid-entries of a recompiled set (entering a function in its middle)."""
    out = []
    if os.path.isfile(path):
        for m in MID_RE.finditer(open(path).read()):
            pm = re.search(r'([0-9A-Fa-f]{8})$', m.group(3))
            if pm:
                out.append((int(m.group(2), 16), int(pm.group(1), 16)))
    return sorted(out)


def word_at(segs, va):
    for sg in segs:
        base = int(sg["p_vaddr"], 16)
        data = base64.b64decode(sg["data_b64"])
        if base <= va and va + 4 <= base + len(data):
            return struct.unpack_from("<I", data, va - base)[0]
    return None


def guard_sites(keep, variant, sites, segs, say_key):
    """Puts a check in front of every instruction that run-time code rewrites (games/dissidia/runtime/xdelta_mods.cpp
    dissidia_xmod_site): when the word in memory is no longer the original, the jump it now holds is followed."""
    for site in sorted(set(sites)):
        orig = word_at(segs, site)
        if orig is None:
            continue
        lab = "L_%08X:\n" % site
        owner = [a for a, (n, b) in variant.items() if lab in b]
        if not owner:
            say("  %s: rewritten instruction 0x%08X lies in no compiled function; not followed" % (say_key, site))
            continue
        prev = word_at(segs, site - 4)
        if prev is not None and (prev >> 26) in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (prev is not None and prev >> 26 == 0 and prev & 63 in (8, 9)):
            say("  %s: rewritten instruction 0x%08X is a delay slot; not followed" % (say_key, site))
            continue
        a = owner[0]
        name, body = keep.get(a) or variant[a]
        nxt = "L_%08X" % (site + 8)
        cont = "goto %s;" % nxt if (nxt + ":") in body else "return;"
        chk = ("    if ((uint32_t)MEM_W(rdram, 0x%08XU) != 0x%08XU) { switch (dissidia_xmod_site(rdram, ctx, 0x%08XU)) "
               "{ case 1: %s case 2: return; default: break; } }\n" % (site, orig, site, cont))
        body = body.replace(lab, lab + chk, 1)
        keep[a] = (name, body)


def compile_module(cx, module, key, blob, prefix, sites=(), extra_mid=()):
    """The functions of one game module (EBOOT or an overlay) whose translation differs when the module holds `blob`
    (the patched file) instead of the retail bytes, plus the functions holding `sites` (instructions run-time code of
    the mod rewrites, guarded) and the mid-entries `extra_mid` (where that code jumps back into the game).
    None when the module cannot be handled."""
    _m, an_name, toml_name, _d, _p = X.CODE[key]
    an_path = os.path.join(cx.work, an_name)
    if not os.path.isfile(an_path):
        say("  %s: %s missing (run the pipeline first); skipped" % (key, an_name))
        return None
    analysis = json.load(open(an_path))
    try:
        new_segs = patched_segments(module, analysis, blob)
    except ValueError as e:
        say("  %s: %s; skipped" % (key, e))
        return None
    force, mid = entries_for(analysis, analysis["segments"], new_segs)
    fl = sorted((int(f["address"], 16), int(f["size"])) for f in analysis["functions"] if f.get("size"))
    fstarts = {a for a, _ in fl}
    for t in extra_mid:                      # only where the module has code: a jump into a function's middle
        own = [a for a, sz in fl if a < t < a + sz]
        if own and t not in fstarts:
            mid.append((t, max(own)))
    force, mid = sorted(set(force)), sorted(set(mid))
    patched = dict(analysis)
    patched["segments"] = new_segs
    tmp = tempfile.mkdtemp(prefix="xmod_", dir=cx.work)
    try:
        pa = os.path.join(tmp, "analysis.json")
        json.dump(patched, open(pa, "w"))
        tt = os.path.join(tmp, "recompile.toml")
        ranges = new_code_ranges(analysis["segments"], new_segs)
        funcs = sorted((int(f["address"], 16), int(f["size"])) for f in analysis["functions"] if f.get("size"))
        starts = {a for a, _ in funcs}
        force = sorted(set(force) | {t for t in pointer_targets(new_segs, ranges) if t not in starts})
        lo_seg = min(int(sg["p_vaddr"], 16) for sg in new_segs)
        hi_seg = max(int(sg["p_vaddr"], 16) + len(base64.b64decode(sg["data_b64"])) for sg in new_segs)
        toml = os.path.join(cx.game, toml_name)
        # the retail module through the very same command and prefix: what differs is what the patch changes
        retail = cx.recompile(module, prefix, an_path, toml, os.path.join(tmp, "retail"), key)
        retail_missing = missing_targets(os.path.join(tmp, "retail"))
        od = os.path.join(tmp, "set")
        # the patched module, then again with every lookup target the patch's code needs and the set lacks (a branch
        # out of a function into code the patch wrote, a jump back into the middle of a function ...)
        for _round in range(6):
            open(tt, "w").write(toml_with(toml, force, mid))
            shutil.rmtree(od, ignore_errors=True)
            variant = cx.recompile(module, prefix, pa, tt, od, key)
            known = set(force) | {e for e, _ in mid}
            extra = sorted(t for t in missing_targets(od) - retail_missing
                           if lo_seg <= t < hi_seg and t % 4 == 0 and t not in known)
            if not extra:
                break
            for t in extra:
                own = [a for a, sz in funcs if a < t < a + sz]
                if own and not any(lo <= t < hi for lo, hi in ranges):
                    mid.append((t, max(own)))       # into unchanged code of a function: continue its frame
                else:
                    force.append(t)
            force, mid = sorted(set(force)), sorted(set(mid))
        else:
            say("  %s: lookup targets still missing after 6 rounds: %s" % (key, ", ".join("0x%08X" % t for t in extra[:8])))
        keep = {addr: nb for addr, nb in variant.items() if retail.get(addr) is None or retail[addr][1] != nb[1]}
        if sites:
            guard_sites(keep, variant, sites, new_segs, key)
        mk = kept_mids(od, {n for n, _ in keep.values()})
        fns = sorted((addr, name) for addr, (name, _) in keep.items()) + sorted((addr, n) for addr, n, _ in mk)
        return {"fns": fns, "batch": "".join(b for _, (_, b) in sorted(keep.items())), "mids": [t for _, _, t in mk],
                "info": "%d function(s) and %d mid-entr%s (%d new entr%s)" % (
                    len(keep), len(mk), "y" if len(mk) == 1 else "ies", len(force) + len(mid), "y" if len(force) + len(mid) == 1 else "ies")}
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def blob_entries(data, addr, extra=()):
    """Entry points of code loaded at run time: its first word, every jal / bal target and pointer word inside it."""
    size = len(data)
    out = {addr} | {t for t in extra if addr <= t < addr + size}
    for o in range(0, size - 3, 4):
        w = struct.unpack_from("<I", data, o)[0]
        va = addr + o
        if (w >> 26) == 3:
            t = ((va + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        elif (w >> 26) == 1 and ((w >> 16) & 31) in (0x10, 0x11):        # bltzal / bgezal (bal)
            t = va + 4 + ((w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)) * 4
            if t == va + 8:      # `bal 1f; nop; 1:` reads the PC into $ra: not a call, the code just goes on
                continue
        elif addr <= w < addr + size and w % 4 == 0:
            t = w
        else:
            continue
        if addr <= t < addr + size:
            out.add(t)
    # a function cannot start in a delay slot: the branch before it would lose its delay slot (a pointer-like data
    # word, or a call target that is not one)
    return sorted(t for t in out if t == addr or t in extra
                  or not has_delay_slot(struct.unpack_from("<I", data, t - 4 - addr)[0]))


def has_delay_slot(w):
    """Whether a MIPS / Allegrex word is a branch or jump (the next word is its delay slot)."""
    op = w >> 26
    if op == 0:
        return (w & 0x3F) in (0x08, 0x09)
    if op == 1:
        return ((w >> 16) & 31) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)
    if 2 <= op <= 7 or 0x14 <= op <= 0x17:
        return True
    return op in (0x11, 0x12) and ((w >> 21) & 31) == 8


def compile_blob(cx, data, addr, prefix, entries):
    """All functions of code loaded at run time to `addr`, recompiled as a module of its own."""
    base = json.load(open(os.path.join(cx.work, "analysis.json")))
    tmp = tempfile.mkdtemp(prefix="xmod_", dir=cx.work)
    try:
        ents = blob_entries(data, addr, entries)
        for _round in range(6):
            funcs = []
            for i, e in enumerate(ents):
                end = ents[i + 1] if i + 1 < len(ents) else addr + len(data)
                funcs.append({"name": "FUN_%08x" % e, "address": "0x%08X" % e, "size": end - e,
                              "is_external": False, "is_thunk": False, "source": "binary_scan"})
            an = {k: base[k] for k in base if k not in ("segments", "functions", "mid_entries", "xrefs", "relocations", "constructors")}
            an.update({"functions": funcs, "mid_entries": [], "xrefs": [], "relocations": [], "constructors": [],
                       "segments": [{"p_vaddr": "0x%08X" % addr, "p_filesz": len(data), "p_memsz": len(data), "p_flags": 7,
                                     "data_b64": base64.b64encode(data).decode()}]})
            pa = os.path.join(tmp, "analysis.json")
            json.dump(an, open(pa, "w"))
            tt = os.path.join(tmp, "recompile.toml")
            open(tt, "w").write("[recompile]\n")
            od = os.path.join(tmp, "set")
            shutil.rmtree(od, ignore_errors=True)
            got = cx.recompile(4, prefix, pa, tt, od, "code at 0x%08X" % addr)
            extra = sorted(t for t in missing_targets(od) if addr <= t < addr + len(data) and t % 4 == 0 and t not in ents)
            if not extra:
                break
            ents = sorted(set(ents) | set(extra))
        mk = kept_mids(od, {n for n, _ in got.values()})
            # where the code jumps into the game (back into the middle of a function it hooked, typically)
        outside = set(t for t in missing_targets(od) if not (addr <= t < addr + len(data)) and t % 4 == 0)
        # every j / jal word of the block into the game, also in pieces no function covers (hook templates the code
        # copies into its trampolines at run time); build-code keeps only targets inside a game function
        for o in range(0, len(data) - 3, 4):
            w = struct.unpack_from("<I", data, o)[0]
            if (w >> 26) in (2, 3):
                t = ((addr + o + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if 0x08804000 <= t < 0x08B63170 and not (addr <= t < addr + len(data)):
                    outside.add(t)
        outside = sorted(outside)
        return got, mk, outside
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def read_dynamic(code_dir):
    """code/dynamic.txt (written by the runtime, xdelta_mods.cpp): {(tail offset, size): {"addr", "file", "writes"}}."""
    out = {}
    p = os.path.join(code_dir, "dynamic.txt")
    if not os.path.isfile(p):
        return out
    lines = [l.split() for l in open(p) if l.strip()]
    for f in lines:
        if f[0] == "blob" and len(f) == 5:
            out[(int(f[1]), int(f[2]))] = {"addr": int(f[3]), "file": f[4], "writes": []}
    for f in lines:
        if f[0] == "write" and len(f) == 6:
            b = out.get((int(f[4]), int(f[5])))
            if b:
                b["writes"].append((int(f[1]), int(f[2]), int(f[3])))
    return out


def retail_module(cx, key):
    if key == "EBOOT.BIN":
        p = (discdir.eboot_elf(cx.work) or os.path.join(cx.work, "EBOOT.dec.elf"))
    else:
        p = os.path.join(discdir.disc_dir(cx.work), "PSP_GAME", "USRDIR", "DATA", key)
    return open(p, "rb").read() if os.path.isfile(p) else None


def with_writes(module, blob, writes):
    """The module file with the words the run-time code wrote into it (module 0 EBOOT ELF, 1..3 overlay ELF)."""
    b = bytearray(blob)
    if module == 0:
        segs = X.elf_segments(bytes(blob))
        for _m, va, w in writes:
            for off, sva, fs, _ms in segs:
                if sva <= va < sva + fs:
                    struct.pack_into("<I", b, off + va - sva, w)
    else:
        for _m, va, w in writes:
            o = X.OVL_FILE_HDR + va - X.OVL_BASE
            if X.OVL_FILE_HDR <= o and o + 4 <= len(b):
                struct.pack_into("<I", b, o, w)
    return bytes(b)


def build(a):
    work = os.path.abspath(a.work)
    mods = os.path.abspath(a.mods or X.default_mods(work))
    out = os.path.abspath(a.out or os.path.join(work, "overlays", "xmods"))
    game = os.path.abspath(a.game or os.path.join(HERE, ".."))
    env = dict(os.environ)
    if a.psprecomp:
        env["PSPRECOMP_BIN"] = os.path.abspath(a.psprecomp)
    cx = Ctx(work, game, env)
    sets, batches, decls, mids, wrappers = [], {}, [], [], []
    guarded = {m: set() for m in range(4)}
    names = ["eboot", "battle", "menu", "extra"]
    for idx, (folder, path, meta) in enumerate(X.xdelta_folders(mods)):
        code = os.path.join(path, "code")
        mods_in = [k for k in X.CODE if os.path.isfile(os.path.join(code, k))]
        dyn = read_dynamic(code)
        if not mods_in and not dyn:
            continue
        stamp = meta.get("code_stamp") or X.code_stamp(path)
        try:                                   # the note the game leaves until a build has the mod's code (status.txt)
            os.remove(os.path.join(path, "status.txt"))
        except OSError:
            pass
        if meta.get("code_stamp") and meta["code_stamp"] != X.code_stamp(path):
            say("xmods: %s: code/ changed since the import; using it as it is now" % folder)
        say("xmods: %s (%s)" % (folder, ", ".join(mods_in + (["code loaded at run time"] if dyn else []))))
        base = "xm%d_%s" % (idx, c_ident(folder)[:24])

        def add(res, module, prefix, blob=(0, 0, 0)):
            batches["batch_%s.cpp" % prefix] = res["batch"]
            mids.extend(res["mids"])
            decls.extend("RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx);" % n for _, n in res["fns"])
            sets.append({"folder": folder, "stamp": stamp, "module": module, "fns": res["fns"], "blob": blob})

        # run-time code: each block compiled as is (it checks memory itself), and what it needs from the game modules
        sites = {m: set() for m in range(4)}
        jumps_in = []
        for k, ((off, size), b) in enumerate(sorted(dyn.items())):
            snap = os.path.join(code, b["file"])
            if not os.path.isfile(snap) or os.path.getsize(snap) != size:
                say("  run-time code %s: snapshot missing; skipped" % b["file"])
                continue
            data = open(snap, "rb").read()
            addr = b["addr"]
            prefix = "%s_dyn%d" % (base, k)
            # the instructions it rewrites jump to entry points of it
            ext = [((w & 0x03FFFFFF) << 2) | (va & 0xF0000000) for _m, va, w in b["writes"]]
            for m, va, _w in b["writes"]:
                sites[m].add(va)
            got, mk, outside = compile_blob(cx, data, addr, prefix, ext)
            jumps_in += outside
            starts = sorted(got)
            fns = []
            for i2, fa in enumerate(starts):   # each function checks memory still holds what it was compiled from
                end = min(starts[i2 + 1] if i2 + 1 < len(starts) else addr + size, fa + 4096)
                wname = "%s_chk_%08X" % (prefix, fa)
                wrappers.append((wname, fa, end - fa, zlib.crc32(data[fa - addr:end - addr]) & 0xFFFFFFFF, addr, addr + size, got[fa][0]))
                fns.append((fa, wname))
            for ea, n2, _t in mk:              # mid-entries too: they run compiled code from the same bytes
                nxt = min([x for x in starts if x > ea] + [addr + size])
                end = min(nxt, ea + 4096)
                wname = "%s_chk_%08X" % (prefix, ea)
                wrappers.append((wname, ea, end - ea, zlib.crc32(data[ea - addr:end - addr]) & 0xFFFFFFFF, addr, addr + size, n2))
                fns.append((ea, wname))
            fns.sort()
            batches["batch_%s.cpp" % prefix] = "".join(bd for _, (_, bd) in sorted(got.items()))
            mids.extend(t for _, _, t in mk)
            decls.extend("RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx);" % n for _, (n, _) in got.items())
            decls.extend("RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx);" % n for _, n, _ in mk)
            sets.append({"folder": folder, "stamp": stamp, "module": 4, "fns": fns, "blob": (off, size, addr)})
            say("  run-time code at 0x%08X (%d B): %d function(s); rewrites %d game instruction(s)"
                % (addr, size, len(got), len(b["writes"])))
        # the game modules: the patch's changes, plus guards on the rewritten instructions and entries for the jumps back
        for key, (module, _a, _t, _d, _p) in X.CODE.items():
            in_mod = [t for t in jumps_in if (X.OVL_BASE <= t < 0x08B63170) == (module != 0)]
            if key not in mods_in and not sites[module] and not in_mod:
                continue
            src = open(os.path.join(code, key), "rb").read() if key in mods_in else retail_module(cx, key)
            if src is None:
                say("  %s: not found; its run-time rewrites are not followed" % key)
                continue
            pre = "%s_%s" % (base, names[module])
            res = compile_module(cx, module, key, src, pre, sites[module], in_mod)
            guarded[module] |= set(sites[module]) if res else set()
            if res:
                add(res, module, pre)
                say("  %s: %s replaced%s" % (key, res["info"], ", %d rewritten instruction(s) guarded" % len(sites[module]) if sites[module] else ""))
    # every game function's extent (start, size) per module, from the retail analysis: the runtime interprets a function
    # whose code in memory is not what was compiled (a patch installed without a rebuild, code rewritten at run time)
    # every compiled game function's extent (start, end of its last instruction) per module, read from the retail
    # build's generated code: the runtime interprets a compiled function whose code in memory is not what was compiled
    # (a patch installed without a rebuild, code rewritten at run time)
    tables, midtabs = {}, {}
    for key, (module, _a, _t, retail_dir, _p) in X.CODE.items():
        tables[module] = compiled_extents(os.path.join(work, retail_dir, "generated"))
        midtabs[module] = compiled_mids(os.path.join(work, retail_dir, "mid_entries.cpp"))
    write_set(out, sets, batches, decls, mids, wrappers, tables, guarded, midtabs)
    say("xmods: %d function set(s) -> %s" % (len(sets), out))
    return 0


def write_set(out, sets, batches, decls, mids, wrappers=(), tables=None, guarded=None, midtabs=None):
    gen = os.path.join(out, "generated")
    os.makedirs(gen, exist_ok=True)
    head = ('// Generated by xdelta_mod.py build-code — do not edit\n#include "recomp.h"\n#include "funcs.h"\n\n'
            'int dissidia_xmod_site(uint8_t* rdram, recomp_context* ctx, uint32_t site);\n\n')
    for fn in os.listdir(gen):
        if fn.startswith("batch_") and fn not in batches:
            os.remove(os.path.join(gen, fn))
    for fn, body in batches.items():
        X.write_if_changed(os.path.join(gen, fn), (head + body).encode())
    X.write_if_changed(os.path.join(out, "funcs.h"), ("#pragma once\n#include \"recomp.h\"\n" + "\n".join(sorted(set(decls))) + "\n").encode())
    X.write_if_changed(os.path.join(out, "mid_entries.cpp"), (head + "\n".join(mids)).encode())
    d = [head, "#include <cstdint>\n\n",
         "// layout shared with games/dissidia/runtime/xdelta_mods.cpp\n",
         "struct DissidiaXmodFn { uint32_t addr; void (*fn)(uint8_t*, recomp_context*); };\n",
         "struct DissidiaXmodSet {\n    const char* folder; const char* stamp; int module; const DissidiaXmodFn* fns; int count;\n"
         "    uint64_t blob_off; uint32_t blob_size; uint32_t blob_addr;\n};\n",
         "void dissidia_xmod_checked(uint32_t addr, uint32_t len, uint32_t crc, uint32_t lo, uint32_t hi,\n"
         "                           void (*fn)(uint8_t*, recomp_context*), uint8_t* rdram, recomp_context* ctx);\n\n"]
    for wname, fa, ln, crc, lo, hi, real in wrappers:
        d.append("static void %s(uint8_t* r, recomp_context* c) { dissidia_xmod_checked(0x%08XU, %du, 0x%08XU, 0x%08XU, 0x%08XU, %s, r, c); }\n"
                 % (wname, fa, ln, crc, lo, hi, real))
    for i, st in enumerate(sets):
        d.append("static const DissidiaXmodFn s_fns%d[] = {\n" % i)
        d.extend("    {0x%08XU, %s},\n" % (addr, name) for addr, name in st["fns"])
        d.append("};\n")
    if sets:
        d.append("static const DissidiaXmodSet s_sets[] = {\n")
        for i, st in enumerate(sets):
            off, size, addr = st["blob"]
            d.append("    {%s, \"%s\", %d, s_fns%d, %d, %dull, %du, 0x%08XU},\n"
                     % (json.dumps(st["folder"]), st["stamp"], st["module"], i, len(st["fns"]), off, size, addr))
        d.append("};\n")
    for m in range(4):
        fl = (tables or {}).get(m, [])
        d.append("static const uint32_t s_ftab%d[] = {%s};\n" % (m, ",".join("0x%X,%d" % (a, n) for a, n in fl) or "0"))
        gs = sorted((guarded or {}).get(m, ()))
        d.append("static const uint32_t s_guard%d[] = {%s};\n" % (m, ",".join("0x%X" % a for a in gs) or "0"))
    for m in range(4):
        ml = sorted((midtabs or {}).get(m, []), key=lambda x: (x[1], x[0]))
        d.append("static const uint32_t s_mid%d[] = {%s};\n" % (m, ",".join("0x%X,0x%X" % (p, e) for e, p in ml) or "0"))
    d.append("const uint32_t* dissidia_xmod_mid_entries(int module, int* count) {   // {parent, entry} pairs, by parent\n    switch (module) {\n")
    for m in range(4):
        d.append("        case %d: *count = %d; return s_mid%d;\n" % (m, len((midtabs or {}).get(m, [])), m))
    d.append("        default: *count = 0; return nullptr;\n    }\n}\n")
    d.append("const uint32_t* dissidia_xmod_functions(int module, int* count) {\n    switch (module) {\n")
    for m in range(4):
        d.append("        case %d: *count = %d; return s_ftab%d;\n" % (m, len((tables or {}).get(m, [])), m))
    d.append("        default: *count = 0; return nullptr;\n    }\n}\n")
    d.append("const uint32_t* dissidia_xmod_guarded_sites(int module, int* count) {\n    switch (module) {\n")
    for m in range(4):
        d.append("        case %d: *count = %d; return s_guard%d;\n" % (m, len((guarded or {}).get(m, ())), m))
    d.append("        default: *count = 0; return nullptr;\n    }\n}\n")
    d.append("\nconst DissidiaXmodSet* dissidia_xmod_sets(int* count) {\n")
    d.append("    *count = %d;\n    return %s;\n}\n" % (len(sets), "s_sets" if sets else "nullptr"))
    X.write_if_changed(os.path.join(out, "dispatch.cpp"), "".join(d).encode())
    X.write_if_changed(os.path.join(out, "overlay.txt"), b"4 xmods\n")
