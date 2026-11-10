#!/usr/bin/env python3
"""Build a platform's release from source: toolchain bundle -> header stub -> runtime core + SDK -> wizard -> assembled release.

  build_release.py --platform linux-x86_64|windows-x86_64 --version V --work DIR
                   [--bundle DIR] [--recompiler FILE] [--jobs N] [--no-archive]

Runs on a Linux host (Windows is cross-built; its recompiler comes from a Windows runner via --recompiler since cargo builds it
natively there). This is what the GitHub workflow runs per platform (.github/workflows/release.yml) and what can be run by hand
to check a release.

  1. bundle      toolchain/build_bundle.py (skipped with --bundle: an existing bundle dir of this platform)
  2. stub        psprecomp recompile --headers-only on the shipped analysis: the four small generated headers the core needs,
                 no game code and no game bytes (CI has no game)
  3. core + SDK  cmake with the bundle's toolchain file; psprecomp_core; cmake --install --component sdk
  4. wizard      setup/ built the same way
  5. assemble    release/assemble.py
"""
import argparse
import gzip
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))


def run(cmd, env=None, cwd=None):
    print("$ " + " ".join(cmd), flush=True)
    r = subprocess.run(cmd, env=env, cwd=cwd)
    if r.returncode != 0:
        print("build_release: %s failed (%d)" % (os.path.basename(cmd[0]), r.returncode), file=sys.stderr)
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--platform", required=True, choices=["linux-x86_64", "windows-x86_64"])
    ap.add_argument("--version", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--bundle")
    ap.add_argument("--recompiler")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--no-archive", action="store_true")
    a = ap.parse_args()
    plat = a.platform
    win = plat.startswith("windows")
    cross = plat != "linux-x86_64"
    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    j = str(a.jobs)

    # 1. bundle
    bundle = os.path.abspath(a.bundle) if a.bundle else os.path.join(work, "bundle-" + plat)
    if not a.bundle:
        run([sys.executable, "-I", os.path.join(REPO, "toolchain", "build_bundle.py"), plat, "--out", bundle,
             "--cache", os.path.join(work, "cache")])
    if cross:
        host = bundle + ".host"
        tcfile = os.path.join(host, "toolchain-cross.cmake")
        tools = [os.path.join(host, "bin"), os.path.join(host, "cmake", "bin")]
    else:
        tcfile = os.path.join(bundle, "toolchain.cmake")
        tools = [os.path.join(bundle, "bin"), os.path.join(bundle, "cmake", "bin")]
    if not os.path.isfile(tcfile):
        sys.exit("build_release: %s not found (is --bundle a complete bundle of %s?)" % (tcfile, plat))
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(tools + [env["PATH"]])
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(work, "zig-cache")

    # the recompiler (a Windows one has to be passed in)
    rec = a.recompiler
    if not rec:
        if cross:
            sys.exit("build_release: pass --recompiler for %s (built natively on a Windows runner)" % plat)
        run(["cargo", "build", "--release", "-p", "psp-cli"], cwd=REPO)
        rec = os.path.join(REPO, "target", "release", "psprecomp")
    rec = os.path.abspath(rec)

    # 2. header stub
    stub = os.path.join(work, "stub")
    shutil.rmtree(stub, ignore_errors=True)
    os.makedirs(stub)
    ana = os.path.join(stub, "analysis.json")
    with gzip.open(os.path.join(REPO, "games", "dissidia", "analysis", "analysis.json.gz")) as g, open(ana, "wb") as f:
        shutil.copyfileobj(g, f)
    stub_env = dict(os.environ, PSPRECOMP_CROSS_MID="1")   # the player's output is generated with it too (psp_build.py)
    host_rec = rec
    if win:   # a Windows exe cannot run here: use the Linux recompiler for the stub (the headers are platform independent)
        host_rec = os.path.join(REPO, "target", "release", "psprecomp")
        if not os.path.isfile(host_rec):
            run(["cargo", "build", "--release", "-p", "psp-cli"], cwd=REPO)
    run([host_rec, "recompile", ana, "--config", os.path.join(REPO, "games", "dissidia", "game.toml"), "-o",
         os.path.join(stub, "out"), "--headers-only"], env=stub_env)

    # 3. core + SDK
    core = os.path.join(work, "core-" + plat)
    sdk = os.path.join(work, "sdk-" + plat)
    shutil.rmtree(core, ignore_errors=True)
    shutil.rmtree(sdk, ignore_errors=True)
    run(["cmake", "-S", os.path.join(REPO, "runtime"), "-B", core, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TOOLCHAIN_FILE=" + tcfile, "-DPSPRECOMP_OUTPUT_DIR=" + os.path.join(stub, "out")], env=env)
    run(["cmake", "--build", core, "-j", j, "--target", "psprecomp_core"], env=env)
    run(["cmake", "--install", core, "--component", "sdk", "--prefix", sdk], env=env)

    # 4. wizard
    wiz = os.path.join(work, "setup-" + plat)
    shutil.rmtree(wiz, ignore_errors=True)
    run(["cmake", "-S", os.path.join(REPO, "setup"), "-B", wiz, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TOOLCHAIN_FILE=" + tcfile, "-DCMAKE_EXE_LINKER_FLAGS=-s"], env=env)
    run(["cmake", "--build", wiz, "-j", j], env=env)
    wizard = os.path.join(wiz, "psprecomp_setup" + (".exe" if win else ""))

    # 5. assemble
    cmd = [sys.executable, "-I", os.path.join(HERE, "assemble.py"), "--platform", plat, "--version", a.version,
           "--out", os.path.join(work, "psprecomp-%s-%s" % (a.version, plat)), "--wizard", wizard, "--recompiler", rec,
           "--toolchain", bundle, "--sdk", sdk, "--cache", os.path.join(work, "cache")]
    if a.no_archive:
        cmd.append("--no-archive")
    run(cmd)


if __name__ == "__main__":
    main()
