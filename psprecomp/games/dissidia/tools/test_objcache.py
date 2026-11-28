#!/usr/bin/env python3
"""Self-test for objcache.py (the object cache of the generated game code) on a made-up work folder: no game data, no compiler
(Python stands in for it). Runs on Linux, macOS and Windows. Run: python3 -I test_objcache.py

What it holds in place, because an update must reuse every object whose code did not change, and never one whose code did:
  - the key does not depend on the work folder or the release folder (paths in the flags, in a response file, the compiler's
    "InstalledDir" line);
  - it changes with the source, a header the source includes, a compiler flag and the compiler, and explain() names which;
  - a header nothing includes (recomp_fingerprint.h: the time of the translation) does not change it;
  - cleanup keeps what the build folder uses, also after a build that had nothing to do, and does nothing with a partial manifest;
  - the launcher: a miss compiles, stores and says why; a hit copies the object and writes the dependency file; an error in the
    cache does not fail the compile; the memory gate registers and unregisters the compile and measures its memory.
"""
import os, shutil, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import objcache as oc


def make_work(root):
    for d in ("output/include", "output/generated", "overlays/battle/include", "overlays/battle/generated"):
        os.makedirs(os.path.join(root, d))

    def w(p, t):
        with open(os.path.join(root, p), "w") as f:
            f.write(t)
    w("output/include/recomp.h", "#include <cmath>\nstruct ctx {};\n")
    w("output/include/recomp_fingerprint.h", "#define T 1\n")
    w("output/funcs.h", "void f();\n")
    w("output/generated/batch_0001.cpp", '#include "recomp.h"\n#include "funcs.h"\nvoid f() {}\n')
    w("overlays/battle/include/recomp.h", "struct ctx {};\n")
    w("overlays/battle/funcs.h", "void g();\n")
    w("overlays/battle/generated/batch_0001.cpp", '#include "recomp.h"\n#include "funcs.h"\nvoid g() {}\n')


def flags(rel, n):
    return ["-O2", "-std=gnu++17", "-I/%s/sdk/include" % rel, "-isystem", "/%s/ndk/sysroot" % rel, "--sysroot=/%s/x/sysroot" % rel,
            "-MD", "-MT", "%s.o" % n, "-MF", "%s.d" % n, "-o", "%s.o" % n, "-c", "%s.cpp" % n]


def append(path, text):
    with open(path, "a") as f:
        f.write(text)
    oc._digests.clear()


def main():
    a, b = tempfile.mkdtemp(), tempfile.mkdtemp()
    saved = dict(os.environ)
    try:
        make_work(a)
        make_work(b)
        S, O = "output/generated/batch_0001.cpp", "overlays/battle/generated/batch_0001.cpp"
        fa, fb = flags("rel-1.0", "x"), flags("rel-2.0", "y")

        def parts(w, s, f, c="cc 1"):
            oc._digests.clear()
            return oc.key_parts(w, os.path.join(w, s), f, c)

        def key(w, s, f, c="cc 1"):
            return oc.key_of(parts(w, s, f, c))
        p0 = parts(a, S, fa)
        base = oc.key_of(p0)
        assert sorted(p0["headers"]) == ["output/funcs.h", "output/include/recomp.h"], p0["headers"]
        assert base == key(b, S, fb), "the key must not depend on the work folder or on folder paths in the flags"
        assert base != key(a, O, fa), "a file of the same name in another module is another object"
        with open(os.path.join(a, "output/include/recomp_fingerprint.h"), "w") as f:
            f.write("#define T 2\n")
        assert base == key(a, S, fa), "a header the source does not include must not change its key"
        o1 = ["-O1" if x == "-O2" else x for x in fa]
        assert base != key(a, S, o1) and oc.explain(p0, parts(a, S, o1)) == "the compiler flags changed"
        assert base != key(a, S, fa, "cc 2") and oc.explain(p0, parts(a, S, fa, "cc 2")) == "the compiler changed"
        assert oc.explain(None, p0) == "first build" and oc.explain(p0, p0) == "its object was not kept"
        append(os.path.join(a, "output/funcs.h"), "void h();\n")
        p2 = parts(a, S, fa)
        k2 = oc.key_of(p2)
        assert base != k2 and oc.explain(p0, p2) == "the header funcs.h changed"
        assert key(a, O, fa) == key(b, O, fb), "the main module's funcs.h is not the overlay's"
        append(os.path.join(a, S), "// x\n")
        assert k2 != key(a, S, fa) and oc.explain(p2, parts(a, S, fa)) == "its code changed"
        assert oc.key(a, os.path.join(a, "elsewhere.cpp"), fa, "cc 1") is None, "only generated code is cached"
        assert oc.key(a, os.path.join(a, S), fa, None) is None, "an unknown compiler: nothing is cached"

        # flags: a response file counts like the command line; a quoted path in a definition is reduced to its last part
        rsp = os.path.join(a, "includes.rsp")
        with open(rsp, "w") as f:
            f.write('-I"/rel 1/inc" -isystem /rel-1/sys -DX=1\n')
        assert oc.normalize_flags(["-O2", "@" + rsp]) == ["-O2", "-DX=1"]
        assert oc.normalize_flags(['-DP="/rel-1/data dir"', "-DQ=/rel-1/q", "/rel-1/tool"]) == ["-DP=data dir", "-DQ=q", "tool"]
        v1 = oc.version_text("clang version 20.1.2 (https://github.com/x/y abc)\nTarget: x86_64\nInstalledDir: /home/u/rel-1.0/zig\n")
        v2 = oc.version_text("clang version 20.1.2 (https://github.com/x/y abc)\nTarget: x86_64\nInstalledDir: C:\\Games\\rel-2.0\\zig\n")
        assert v1 == v2 and "clang version 20.1.2" in v1 and "Target" in v1, "the compiler's folder is not part of its identity"

        # store / lookup / cleanup with a manifest
        cache = os.path.join(a, "cache")
        man = os.path.join(a, "build", "objcache.manifest")
        obj = os.path.join(a, "build", "x.o")
        os.makedirs(os.path.dirname(obj))
        with open(obj, "w") as f:
            f.write("OBJ")
        oc.store(cache, base, obj)
        oc.store(cache, k2, obj)
        with open(oc.lookup(cache, base)) as f:
            assert f.read() == "OBJ"
        os.environ["PSPRECOMP_OBJCACHE_MANIFEST"] = man
        os.environ["PSPRECOMP_OBJCACHE_BUILD"] = "run 1"
        oc.note_object(obj, k2, "miss", "first build")
        oc.note_object(obj, base, "hit")   # the object was made again: its last note wins
        r = oc.prune_build(cache, man, min_objects=5, build="run 1")
        assert (r["kept"], r["removed"], r["hit"], r["miss"]) == (1, 0, 1, 0) and oc.lookup(cache, k2), "a partial manifest removes nothing"
        r = oc.prune_build(cache, man, build="run 2")
        assert (r["kept"], r["removed"], r["hit"], r["miss"]) == (1, 1, 0, 0) and oc.lookup(cache, base) and not oc.lookup(cache, k2)
        assert oc.prune_build(cache, man)["removed"] == 0 and oc.lookup(cache, base), "a build with nothing to do keeps the entry"
        assert oc.prune(cache, []) == (0, 0) and oc.lookup(cache, base), "no manifest: nothing is removed"
        os.remove(obj)
        assert oc.prune_build(cache, man)["kept"] == 0 and not os.listdir(man) and oc.lookup(cache, base), "an object that is gone is forgotten; nothing known, nothing removed"
        assert oc.reasons_text({"the compiler changed": 468}) == "the compiler changed"
        assert oc.reasons_text({"its code changed": 3, "first build": 1}) == "its code changed (3), first build (1)"

        # the rule of the memory gate
        gb = 1024 * 1024
        assert oc.may_start(0, 0, 4 * gb, 0), "one compile always runs"
        assert not oc.may_start(1, 0, 1 * gb, 1 * gb) and oc.may_start(1, 0, 1 * gb, 2 * gb)
        assert not oc.may_start(2, 2, 1 * gb, 3 * gb) and oc.may_start(2, 2, 1 * gb, 4 * gb), "young compiles are counted again"
        assert oc.estimate_kb(0) == oc.FIRST_EST_KB and oc.estimate_kb(400 * 1024) == 500 * 1024
        free = oc.mem_free_kb()
        assert free is None or free > 0

        # the launcher. The stand-in compiler is Python itself running cc.py: it copies the source to the object, writes a
        # dependency file, notes that it ran and uses about 150 MB.
        cc = os.path.join(a, "cc.py")
        with open(cc, "w") as f:
            f.write("import sys\na = sys.argv\nsrc, obj, dep = a[a.index('-c') + 1], a[a.index('-o') + 1], a[a.index('-MF') + 1]\n"
                    "b = bytearray(150 << 20)\nb[::4096] = b'\\x01' * len(b[::4096])\n"
                    "open(obj, 'w').write(open(src).read())\nopen(dep, 'w').write(obj.replace(' ', '\\\\ ') + ': ' + src + ' /some/header.h\\n')\n"
                    "open(obj + '.ran', 'a').write('x')\n")
        src = os.path.join(a, S)
        o1, o2 = os.path.join(a, "b 1", "t.o"), os.path.join(a, "b 2", "t.o")   # folders with a space, as a Windows user folder has
        os.makedirs(os.path.dirname(o1))
        os.makedirs(os.path.dirname(o2))
        cache2 = os.path.join(a, "cache2")
        os.environ.update({"PSPRECOMP_OBJCACHE": cache2, "PSPRECOMP_OBJCACHE_WORK": a, "PSPRECOMP_OBJCACHE_MANIFEST": os.path.join(a, "man2"),
                           "PSPRECOMP_OBJCACHE_BUILD": "run 3", "PSPRECOMP_OBJCACHE_GATE": "1"})
        oc._digests.clear()
        comp = [sys.executable, cc]
        assert oc.launch(comp + ["-O2", "-I/rel-1/inc", "-MD", "-MT", o1, "-MF", o1 + ".d", "-o", o1, "-c", src]) == 0
        assert oc.launch(comp + ["-O2", "-I/rel-2/inc", "-MD", "-MT", o2, "-MF", o2 + ".d", "-o", o2, "-c", src]) == 0
        with open(o1) as f1, open(o2) as f2, open(src) as f3:
            assert f1.read() == f2.read() == f3.read()
        assert os.path.isfile(o1 + ".ran") and not os.path.isfile(o2 + ".ran"), "the second compile is a hit: the compiler does not run"
        with open(o2 + ".d") as f:
            assert f.read() == o2.replace(" ", "\\ ") + ": " + src + " /some/header.h\n", "a hit writes the dependency file for its own object"
        r = oc.prune_build(cache2, os.environ["PSPRECOMP_OBJCACHE_MANIFEST"], build="run 3")
        assert (r["hit"], r["miss"], r["reasons"]) == (1, 1, {"first build": 1}), r
        g = oc.Gate(cache2)
        assert not [n for n in os.listdir(g.dir) if n[0].isdigit()], "the gate's job files are removed"
        if hasattr(os, "wait4") or os.name == "nt":
            assert g.peak_kb() > 100 * 1024, "the compile's memory is measured (%d KB)" % g.peak_kb()
        oc.gate_reset(cache2)
        assert oc.Gate(cache2).peak_kb() == 0
        append(src, "// y\n")
        assert oc.launch(comp + ["-O2", "-MD", "-MT", o2, "-MF", o2 + ".d", "-o", o2, "-c", src]) == 0
        with open(o2) as f:
            assert os.path.isfile(o2 + ".ran") and f.read().endswith("// y\n"), "a changed source is compiled"
        r = oc.prune_build(cache2, os.environ["PSPRECOMP_OBJCACHE_MANIFEST"], build="run 3")
        assert r["reasons"] == {"first build": 1, "its code changed": 1}, r

        # an error in the cache must not fail the compile: the cache folder is a file here
        bad = os.path.join(a, "notafolder")
        with open(bad, "w") as f:
            f.write("x")
        os.environ["PSPRECOMP_OBJCACHE"] = bad
        o3 = os.path.join(a, "b 1", "u.o")
        assert oc.launch(comp + ["-O2", "-MD", "-MT", o3, "-MF", o3 + ".d", "-o", o3, "-c", src]) == 0 and os.path.isfile(o3 + ".ran")
        # a failing compiler's exit code is passed on
        assert oc.launch([sys.executable, "-c", "import sys; sys.exit(3)", "-o", o3, "-c", src]) == 3
    finally:
        os.environ.clear()
        os.environ.update(saved)
        shutil.rmtree(a, ignore_errors=True)
        shutil.rmtree(b, ignore_errors=True)
    print("ok")


if __name__ == "__main__":
    main()
