#!/usr/bin/env python3
"""Build the Android setup app (the APK that builds the game on the phone) from source, on a Linux host.

  build_android_app.py --sdk ANDROID_SDK --work DIR --version V [--jobs N] [-o APK]

ANDROID_SDK has ndk/<version of android/versions.json>, build-tools/* and platforms/android-* (sdkmanager installs them; a JDK is needed for
javac/d8). Also needed on PATH: cargo with the aarch64-unknown-linux-musl target, cmake, ninja, python3, perl, make, patchelf (pip install patchelf).
Everything downloaded is checked against a pinned sha256 (toolchain/versions.json, android/versions.json). Nothing here is game-derived.

  1. the Android part         release/build_android_sdk.py: prebuilt core + SDL2/FreeType/FFmpeg for arm64 with the NDK, APK skeleton
  2. recompiler               psprecomp for aarch64-linux-musl (static; runs from the app's lib folder), linked with zig
  3. pspdecrypt               static aarch64, built with zig against pinned OpenSSL + zlib
  4. setup app skeleton       SDL's Java activity + android/setup/ (manifest, SetupActivity)
  5. wizard                   setup/ as libmain.so (android/build_setup_app.py: zig + the NDK sysroot)
  6. pack                     android/pack_setup_app.py: the APK, with the tools as lib*.so and the payload zip
"""
import argparse
import glob
import gzip
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))


def run(cmd, env=None, cwd=None):
    print("$ " + " ".join(cmd), flush=True)
    if subprocess.run(cmd, env=env, cwd=cwd).returncode != 0:
        sys.exit("build_android_app: %s failed" % os.path.basename(cmd[0]))


def load_bundle_module():
    spec = importlib.util.spec_from_file_location("build_bundle", os.path.join(REPO, "toolchain", "build_bundle.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def untar(archive, dest, strip=0):
    os.makedirs(dest, exist_ok=True)
    with tarfile.open(archive) as t:
        members = t.getmembers()
        if strip:
            for m in members:
                m.name = m.name.split("/", strip)[-1] if "/" in m.name else m.name
            members = [m for m in members if m.name and m.name != "."]
        t.extractall(dest, members=members, filter="data")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sdk", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    work = os.path.abspath(a.work)
    sdk = os.path.abspath(a.sdk)
    os.makedirs(work, exist_ok=True)
    j = str(a.jobs)
    bb = load_bundle_module()
    tv = json.load(open(os.path.join(REPO, "toolchain", "versions.json")))
    av = json.load(open(os.path.join(REPO, "android", "versions.json")))
    cache = os.path.join(work, "cache")
    os.makedirs(cache, exist_ok=True)
    ndk = os.path.join(sdk, "ndk", av["ndk"]["version"])
    if not os.path.isdir(ndk):
        sys.exit('build_android_app: NDK %s not found under %s (sdkmanager "ndk;%s")' % (av["ndk"]["version"], sdk, av["ndk"]["version"]))
    for tool in ("cargo", "cmake", "ninja", "perl", "make", "patchelf", "java"):
        if not shutil.which(tool):
            sys.exit("build_android_app: %s not found in PATH" % tool)

    # ---- inputs: every pinned source and tool, verified
    for name, entry in tv["sources"].items():
        bb.fetch(entry, cache)
    zig_host = os.path.join(work, "zig-host")
    if not os.path.isfile(os.path.join(zig_host, "zig")):
        tar = bb.fetch(tv["zig"]["files"]["linux-x86_64"], cache)
        untar(tar, zig_host, strip=1)
    zig = os.path.join(zig_host, "zig")
    zig_a64 = os.path.join(work, "zig-aarch64")
    if not os.path.isfile(os.path.join(zig_a64, "zig")):
        untar(bb.fetch(av["tools"]["zig_aarch64"], cache), zig_a64, strip=1)
    py_tree = os.path.join(work, "python-aarch64")
    if not os.path.isdir(os.path.join(py_tree, "bin")):
        untar(bb.fetch(av["tools"]["python"], cache), py_tree, strip=1)
    musl = os.path.join(work, "ld-musl-aarch64.so.1")
    if not os.path.isfile(musl):
        ent = av["tools"]["musl_loader"]
        with tarfile.open(bb.fetch(ent, cache)) as t, open(musl, "wb") as f:
            f.write(t.extractfile(ent["member"]).read())
    zenv = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(work, "zig-cache"))

    # ---- 1. the Android part (needs the host recompiler's header stub)
    rec_host = os.path.join(REPO, "target", "release", "psprecomp")
    run(["cargo", "build", "--release", "-p", "psp-cli"], cwd=REPO)
    stub = os.path.join(work, "stub")
    shutil.rmtree(stub, ignore_errors=True)
    os.makedirs(stub)
    with gzip.open(os.path.join(REPO, "games", "dissidia", "analysis", "analysis.json.gz")) as g, open(os.path.join(stub, "analysis.json"), "wb") as f:
        shutil.copyfileobj(g, f)
    run([rec_host, "recompile", os.path.join(stub, "analysis.json"), "--config", os.path.join(REPO, "games", "dissidia", "game.toml"),
         "-o", os.path.join(stub, "out"), "--headers-only"], env=dict(os.environ, PSPRECOMP_CROSS_MID="1"))
    part = os.path.join(work, "android-part")
    if not os.path.isfile(os.path.join(part, "android.json")):
        run([sys.executable, "-I", os.path.join(HERE, "build_android_sdk.py"), "--sdk", sdk, "--cache", cache, "--cmake", shutil.which("cmake"),
             "--ninja", shutil.which("ninja"), "--stub", os.path.join(stub, "out"), "--work", os.path.join(work, "android-work"), "--out", part])

    # ---- 2. the recompiler for the phone: static aarch64 musl, linked with zig (it adds crt + libc itself, so Rust's own are switched off)
    wrap = os.path.join(work, "zigcc-aarch64.sh")
    with open(wrap, "w") as f:
        f.write('#!/bin/sh\nfor a in "$@"; do case "$a" in -Wl,--fix-cortex-a53-843419) ;; *) set -- "$@" "$a";; esac; shift; done\n'
                'exec "%s" cc -target aarch64-linux-musl "$@"\n' % zig)
    os.chmod(wrap, 0o755)
    renv = dict(zenv, CC_aarch64_unknown_linux_musl=wrap, CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=wrap, RUSTFLAGS="-C link-self-contained=no")
    run(["cargo", "build", "--release", "-p", "psp-cli", "--target", "aarch64-unknown-linux-musl"], env=renv, cwd=REPO)
    recompiler = os.path.join(REPO, "target", "aarch64-unknown-linux-musl", "release", "psprecomp")

    # ---- 3. pspdecrypt: libcrypto (SHA-256, DES) and zlib, static, with zig for aarch64-linux-musl
    pdw = os.path.join(work, "pspdecrypt-build")
    pspdecrypt = os.path.join(pdw, "pspdecrypt")
    if not os.path.isfile(pspdecrypt):
        shutil.rmtree(pdw, ignore_errors=True)
        for n in ("pd", "ssl", "z", "obj", "bin"):
            os.makedirs(os.path.join(pdw, n))
        untar(os.path.join(cache, tv["sources"]["pspdecrypt"]["url"].rsplit("/", 1)[1]), os.path.join(pdw, "pd"), strip=1)
        untar(os.path.join(cache, tv["sources"]["openssl"]["url"].rsplit("/", 1)[1]), os.path.join(pdw, "ssl"), strip=1)
        untar(os.path.join(cache, tv["sources"]["zlib"]["url"].rsplit("/", 1)[1]), os.path.join(pdw, "z"), strip=1)
        tools = {}
        for name, sub in (("cc", "cc"), ("cxx", "c++"), ("ar", "ar"), ("ranlib", "ranlib")):
            p = os.path.join(pdw, "bin", name)
            with open(p, "w") as f:
                f.write('#!/bin/sh\nexec "%s" %s%s "$@"\n' % (zig, sub, " -target aarch64-linux-musl" if name in ("cc", "cxx") else ""))
            os.chmod(p, 0o755)
            tools[name] = p
        ssl, zl = os.path.join(pdw, "ssl"), os.path.join(pdw, "z")
        run([os.path.join(ssl, "Configure"), "linux-aarch64", "no-shared", "no-asm", "no-tests", "no-threads", "no-dso", "no-engine", "no-module",
             "no-legacy", "no-autoload-config", "no-ssl", "no-tls", "no-dtls", "no-comp", "no-ocsp", "no-cms", "no-ts", "no-ct", "no-srp", "no-psk",
             "--prefix=" + os.path.join(pdw, "inst"), "--libdir=lib", "CC=" + tools["cc"], "AR=" + tools["ar"], "ARFLAGS=r", "RANLIB=" + tools["ranlib"],
             "CFLAGS=-fno-sanitize=undefined"], env=zenv, cwd=ssl)
        run(["make", "-j", j, "build_libs"], env=zenv, cwd=ssl)
        run(["./configure", "--static"], env=dict(zenv, CC=tools["cc"], AR=tools["ar"], RANLIB=tools["ranlib"]), cwd=zl)
        run(["make", "-j", j, "libz.a"], env=zenv, cwd=zl)
        psrc = os.path.join(pdw, "pd")
        objs = []
        for sub in ("", "libkirk"):
            for fn in sorted(os.listdir(os.path.join(psrc, sub) if sub else psrc)):
                base, ext = os.path.splitext(fn)
                if ext not in (".c", ".cpp") or (not sub and base == "kl4e_test"):
                    continue
                o = os.path.join(pdw, "obj", (sub + "_" if sub else "") + base + ".o")
                run([tools["cc"] if ext == ".c" else tools["cxx"], "-O3", "-fno-sanitize=undefined", "-I" + ssl + "/include", "-I" + zl, "-I" + psrc,
                     "-c", os.path.join(psrc, sub, fn), "-o", o], env=zenv)
                objs.append(o)
        run([tools["cxx"], "-o", pspdecrypt, "-s"] + objs + [os.path.join(zl, "libz.a"), os.path.join(ssl, "libcrypto.a")], env=zenv)
        shutil.copy2(os.path.join(psrc, "LICENSE.TXT"), os.path.join(work, "pspdecrypt-LICENSE.txt"))

    # ---- 4. the setup app's skeleton (SDL's Java activity + our SetupActivity)
    skeleton = os.path.join(work, "setup_skeleton.apk")
    run([sys.executable, "-I", os.path.join(REPO, "android", "make_skeleton.py"), "--sdk", sdk, "--sdl-src", os.path.join(work, "android-work", "deps", "src-sdl2"),
         "--package", "org.psprecomp.setup", "--label", "Dissidia Recomp Setup", "--manifest", os.path.join(REPO, "android", "setup", "AndroidManifest.xml"),
         "--extra-java", os.path.join(REPO, "android", "setup", "java"), "--api", str(av["api"]), "--version", a.version, "-o", skeleton])

    # ---- 5. the wizard
    libmain = os.path.join(work, "setup_libmain.so")
    run([sys.executable, "-I", os.path.join(REPO, "android", "build_setup_app.py"), "--zig", zig, "--ndk", ndk, "--deps", os.path.join(part, "deps", "deps"),
         "--out", libmain, "--jobs", j], env=zenv)

    # ---- 6. the APK
    out = a.output or os.path.join(work, "DissidiaRecomp-Setup-%s-android.apk" % a.version)
    run([sys.executable, "-I", os.path.join(REPO, "android", "pack_setup_app.py"), "--android-part", part, "--ndk", ndk, "--zig-tree", zig_a64,
         "--python-tree", py_tree, "--musl", musl, "--psprecomp", recompiler, "--pspdecrypt", pspdecrypt, "--skeleton", skeleton, "--libmain", libmain,
         "-o", out])
    import hashlib
    h = hashlib.sha256(open(out, "rb").read()).hexdigest()
    open(out + ".sha256", "w").write("%s  %s\n" % (h, os.path.basename(out)))
    print("built %s (%.0f MB) sha256 %s" % (out, os.path.getsize(out) / 1e6, h))


if __name__ == "__main__":
    main()
