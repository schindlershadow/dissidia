#!/usr/bin/env python3
"""Assemble the Android setup app (APK): the wizard, the tools it runs on the phone, and the payload they work from.

  pack_setup_app.py --android-part DIR --ndk NDK --zig-tree DIR --python-tree DIR --musl FILE --psprecomp FILE --pspdecrypt FILE
                    --skeleton APK --libmain SO [--game DIR] [--patchelf PATCHELF] -o setup.apk

Executables go into lib/arm64-v8a/ as lib*.so (the only place an app may run files from since Android 10): libzig.so (zig 0.15, static,
clang + lld in one), libpsprecomp.so (the recompiler, static musl), libpspdecrypt.so, libpython.so (python-build-standalone, musl; its
interpreter is patched to /data/data/org.psprecomp.setup/files/ld-musl, a link the app makes to libmusl.so = Alpine's musl loader), libmain.so
(the wizard) and libSDL2.so. Everything else (Python's library, zig's lib, the NDK sysroot, the Android core + libraries, the game's tools
and analysis) is one zip, lib/arm64-v8a/libpayload.so, that the wizard unpacks on its first start. Standard library only (+ patchelf).
"""
import argparse, hashlib, os, shutil, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SYSROOT = "toolchains/llvm/prebuilt/linux-x86_64/sysroot"
CLANG_RT = "toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux"
INTERP = "/data/data/org.psprecomp.setup/files/ld-musl"


def add_tree(z, src, arc, skip=lambda rel: False):
    for dp, dns, fns in os.walk(src):
        dns[:] = sorted(d for d in dns if not skip(os.path.relpath(os.path.join(dp, d), src).replace("\\", "/") + "/"))
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            rel = os.path.relpath(p, src).replace("\\", "/")
            if skip(rel) or os.path.islink(p):
                continue
            z.write(p, arc + "/" + rel)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    for n in ("android-part", "ndk", "zig-tree", "python-tree", "musl", "psprecomp", "pspdecrypt", "skeleton", "libmain"):
        ap.add_argument("--" + n, required=True)
    ap.add_argument("--game", default=os.path.join(REPO, "games", "dissidia"))
    ap.add_argument("--patchelf", default="patchelf")
    ap.add_argument("--update-key", help="the public key the updater accepts manifests for (default: games/dissidia/update_key.pub); tests only")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    work = tempfile.mkdtemp(prefix="setupapk-")

    # what the translated + compiled game depends on: when it differs from the fingerprint of the last build on the phone, the game is built again
    fp_inputs = []

    # ---- the payload zip
    payload = os.path.join(work, "payload.zip")
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        # Python's library (no tests, GUI toolkits, pip: not needed by the build tools)
        pyskip = lambda rel: rel.startswith(("bin/", "share/", "include/", "lib/itcl", "lib/tcl", "lib/tk", "lib/thread", "lib/libpython", "lib/python3.13/test/",
                                             "lib/python3.13/idlelib/", "lib/python3.13/tkinter/", "lib/python3.13/ensurepip/", "lib/python3.13/site-packages/",
                                             "lib/python3.13/turtledemo/", "lib/python3.13/config-")) or rel.endswith(("libtcl9.0.so", "libtk9.0.so", "_tkinter.cpython-313-aarch64-linux-musl.so")) or "/__pycache__/" in "/" + rel or rel.endswith(".pyc")
        add_tree(z, a.python_tree, "python", pyskip)
        ap_ = a.android_part
        add_tree(z, os.path.join(ap_, "sdk"), "android/sdk")
        add_tree(z, os.path.join(ap_, "deps"), "android/deps")
        add_tree(z, os.path.join(ap_, "apk"), "android/apk")
        for f in ("versions.json", "android.json", "libc++_shared.so", "shaders.pack"):
            if os.path.isfile(os.path.join(ap_, f)):
                z.write(os.path.join(ap_, f), "android/" + f)
        # zig's own library directory: its clang headers and std (the compiler looks for them next to ZIG_LIB_DIR)
        for sub in ("include", "std"):
            add_tree(z, os.path.join(a.zig_tree, "lib", sub), "android/zig-lib/" + sub)
        # the NDK sysroot, arm64 only: headers (with libc++) and the libraries of the pinned API level
        sysroot = os.path.join(a.ndk, SYSROOT)
        add_tree(z, os.path.join(sysroot, "usr", "include"), "android/ndk/" + SYSROOT + "/usr/include")
        lib = os.path.join(sysroot, "usr", "lib", "aarch64-linux-android")
        api = None
        import json
        api = str(json.load(open(os.path.join(ap_, "versions.json")))["api"])
        for fn in sorted(os.listdir(lib)):
            if os.path.isfile(os.path.join(lib, fn)):
                z.write(os.path.join(lib, fn), "android/ndk/" + SYSROOT + "/usr/lib/aarch64-linux-android/" + fn)
        add_tree(z, os.path.join(lib, api), "android/ndk/" + SYSROOT + "/usr/lib/aarch64-linux-android/" + api)
        rt = os.path.join(a.ndk, CLANG_RT)
        for src, name in ((rt + "/libclang_rt.builtins-aarch64-android.a", "libclang_rt.builtins-aarch64-android.a"),
                          (rt + "/aarch64/libunwind.a", "libunwind.a"), (rt + "/aarch64/libatomic.a", "libatomic.a")):
            z.write(src, "android/ndk/clang-rt/" + name)
        z.writestr("android/ndk/.psprecomp-ndk", json.load(open(os.path.join(ap_, "versions.json")))["ndk"]["version"] + "\n")
        # the game's tools and analysis (the same whitelist as release/assemble.py)
        for dp, dns, fns in os.walk(a.game):
            rel_dir = os.path.relpath(dp, a.game).replace("\\", "/")
            top = rel_dir.split("/")[0]
            dns[:] = [d for d in dns if d not in ("__pycache__", ".venv")]
            for fn in fns:
                rel = os.path.normpath(os.path.join(rel_dir, fn)).replace("\\", "/")
                keep = (top == "analysis" and not rel.endswith(".py")) or rel in ("game.toml", "features.ini", "package_paths.txt") or \
                    (rel.startswith("overlay_") and rel.endswith(".toml")) or \
                    (top == "tools" and rel.count("/") == 1 and rel.endswith((".py", ".json", ".txt")) and not fn.startswith("test_"))
                if keep:
                    z.write(os.path.join(dp, fn), "game/" + rel)
                    fp_inputs.append(("game/" + rel, os.path.join(dp, fn)))
        keyfile = a.update_key or os.path.join(a.game, "update_key.pub")   # the updater only accepts a manifest signed with this key
        if os.path.isfile(keyfile):
            z.write(keyfile, "game/update_key.pub")
        fp_inputs.append(("libpsprecomp.so", a.psprecomp))
        fp_inputs.append(("zig", os.path.join(a.zig_tree, "zig")))
        fp_inputs.append(("android/versions.json", os.path.join(ap_, "versions.json")))
        fp_inputs.append(("core", os.path.join(ap_, "sdk", "lib", "libpsprecomp_core.a")))
        for fn in ("libSDL2.so", "libfreetype.a", "libavcodec.a", "libavutil.a"):
            q = os.path.join(ap_, "deps", "deps", "lib", fn)
            if os.path.isfile(q):
                fp_inputs.append(("deps/" + fn, q))
        h = hashlib.sha256()
        for name, path in sorted(fp_inputs):
            fh = hashlib.sha256()
            with open(path, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    fh.update(chunk)
            h.update(name.encode() + b"\0" + fh.hexdigest().encode() + b"\0")
        fingerprint = h.hexdigest()
        z.writestr("android/build_fingerprint.txt", fingerprint + "\n")

    # ---- the python executable with the app's loader as interpreter
    py = os.path.join(work, "libpython.so")
    shutil.copy2(os.path.join(a.python_tree, "bin", "python3.13"), py)
    subprocess.check_call([a.patchelf, "--set-interpreter", INTERP, py])

    libs = [("libmain.so", a.libmain), ("libSDL2.so", os.path.join(a.android_part, "deps", "deps", "lib", "libSDL2.so")),
            ("libzig.so", os.path.join(a.zig_tree, "zig")), ("libpsprecomp.so", a.psprecomp), ("libpspdecrypt.so", a.pspdecrypt),
            ("libpython.so", py), ("libmusl.so", a.musl), ("libpayload.so", payload)]
    cmd = [sys.executable, "-I", os.path.join(HERE, "apk_pack.py"), "--skeleton", a.skeleton, "--key", os.path.join(a.android_part, "apk", "debug_key.json"), "-o", a.output]
    for name, path in libs:
        cmd += ["--lib", "lib/arm64-v8a/%s=%s" % (name, path)]
    subprocess.check_call(cmd)
    open(a.output + ".fp", "w").write(fingerprint + "\n")   # the manifest carries it (release/make_manifest.py)
    shutil.rmtree(work, ignore_errors=True)
    print("setup app:", a.output, "%.0f MB" % (os.path.getsize(a.output) / 1e6))


if __name__ == "__main__":
    main()
