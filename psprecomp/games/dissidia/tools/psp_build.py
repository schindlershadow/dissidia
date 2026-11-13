#!/usr/bin/env python3
"""Build the player's game from an extracted work folder (the step after setup_from_iso.py).

The same four steps as pipeline.sh, in Python so a Windows install needs no bash, and with progress lines a wizard can read:

  1. analysis   hydrate the shipped analysis metadata with the bytes of the player's own binaries (hash-checked)
  2. recompile  psprecomp recompile -> <work>/output, then the four overlay/module sets -> <work>/overlays
  3. configure  cmake, from either an installed SDK (a release: prebuilt runtime core, only the generated C++ is compiled)
                or the source tree's runtime/ (developers)
  4. build      cmake --build -> <work>/build/psprecomp_runtime

  psp_build.py <workdir> [--sdk DIR | --repo DIR] [--game DIR] [--psprecomp BIN] [--toolchain DIR]
                         [--jobs N] [--force] [--progress]

  --sdk DIR       an installed SDK (cmake --install <build> --component sdk): lib/libpsprecomp_core.a + CMakeLists.txt
  --repo DIR      the psprecomp source tree (default: the tree this script is in); used when --sdk is not given
  --game DIR      games/dissidia (game.toml, overlay_*.toml, analysis/, tools/); default: next to this script
  --psprecomp BIN the recompiler (default: <repo>/target/release/psprecomp, built with cargo when missing and cargo exists)
  --toolchain DIR the portable toolchain bundle of the release (toolchain/build_bundle.py: zig clang + libc++, cmake, ninja,
                  static SDL2/FreeType/zlib/libavcodec). Used through its toolchain.cmake, so no system compiler or library is
                  needed. An SDK built with a bundle (it carries toolchain.json) is only linked with the identical bundle.
  --allow-system-compiler  build an SDK that carries a toolchain.json with the system compiler anyway (will not link unless the
                  system compiler happens to be the same one)
  --android [DIR] build the Android APK (<work>/android/dissidia.apk) instead of the desktop binary: needs the release's android/ folder
                  (default: next to --game's parent) and downloads Google's NDK once (games/dissidia/tools/android_build.py).
                  On a phone (the setup app) nothing is downloaded: the bundled zig compiles against the sysroot of the Android part.
  --progress      print machine-readable lines:  [psp-build] STEP <i>/<n> <name>   [psp-build] PROGRESS <percent>
                  [psp-build] DONE <binary>   [psp-build] ERROR <message>

Resumable: step 2 is skipped when <work>/.recompile.stamp matches a fingerprint of its inputs (same as pipeline.sh); the compile
resumes where it stopped. Python 3 standard library only.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ANALYSES = ["analysis", "analysis_battle", "analysis_menu", "analysis_extra", "analysis_supacc"]
STEPS = ["analysis", "recompile", "overlays", "configure", "build"]
PROGRESS = False


def say(msg):
    print(msg, flush=True)


def tag(kind, text=""):
    if PROGRESS:
        print("[psp-build] %s %s" % (kind, text), flush=True)


def die(msg):
    tag("ERROR", msg)
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def step(i, name):
    tag("STEP", "%d/%d %s" % (i, len(STEPS), name))
    say("\n=== %s" % name)


def sha256_files(paths):
    h = hashlib.sha256()
    for p in paths:
        with open(p, "rb") as f:
            h.update(hashlib.sha256(f.read()).hexdigest().encode())
    return h.hexdigest()


def run(cmd, env=None, cwd=None, filt=None):
    """Run a command, streaming its output; `filt(line)` sees every line (progress parsing). Dies on failure."""
    say("$ " + " ".join(cmd))
    # Windows' CreateProcess looks a bare name up in the PARENT's PATH, not in env["PATH"] (where the bundle's cmake lives)
    exe = shutil.which(cmd[0], path=(env or os.environ).get("PATH"))
    if exe:
        cmd = [exe] + list(cmd[1:])
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    for line in p.stdout:
        sys.stdout.write(line)
        sys.stdout.flush()
        if filt:
            filt(line)
    if p.wait() != 0:
        die("%s failed (exit %d)" % (os.path.basename(cmd[0]), p.returncode))


def android_build(a, work, env, jobs, exe):
    """Steps 4-5 for Android: NDK (downloaded once), configure + compile the generated game against the release's Android core, pack the APK."""
    sys.path.insert(0, HERE)
    import android_build as ab
    adir = os.path.abspath(a.android if a.android != "auto" else os.path.join(os.path.abspath(a.game), "..", "android"))
    if not os.path.isdir(adir):
        die("%s: no Android part in this release" % adir)
    ab.check_prebuilt(adir, die)
    if ab.is_device():
        step(4, "Preparing the Android toolchain (the compiler that ships with the app)")
        step(5, "Compiling the game on this device (the long step)")
        lib = ab.device_build(adir, work, jobs, say, tag, die)
        apk = ab.pack_apk(adir, work, lib, run, say, env)
        tag("DONE", apk)
        say("\nDone: %s" % apk)
        return
    step(4, "Preparing the Android toolchain (Google's NDK, downloaded once)")
    ab.fetch_ndk(adir, say, tag, die)
    bd, tc = ab.write_toolchain(adir, work)
    cache = os.path.join(bd, "CMakeCache.txt")
    if os.path.isfile(cache) and os.path.realpath(re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", open(cache).read(), re.M).group(1)) != os.path.realpath(os.path.join(adir, "sdk")):
        shutil.rmtree(bd + os.sep + "CMakeFiles", ignore_errors=True)
        os.remove(cache)
    # Zip archives carry no time zone: a release built in UTC and unpacked west of it has files dated in the FUTURE, so ninja
    # regenerates build.ninja forever ("still dirty after 100 tries"). Pull any future timestamp of the build inputs back to now.
    now = time.time()
    fixed = 0
    for top in {src, os.path.join(repo, "toolchain") if not a.toolchain else os.path.abspath(a.toolchain), game, os.path.join(work, "output"),
                os.path.join(work, "overlays")}:
        for dp, dns, fns in os.walk(top):
            for fn in fns:
                fp = os.path.join(dp, fn)
                try:
                    if os.stat(fp).st_mtime > now:
                        os.utime(fp, (now, now))
                        fixed += 1
                except OSError:
                    pass
    if fixed:
        say("Fixed %d files dated in the future (time zone of the archive)" % fixed)
    ovl = ";".join(os.path.join(work, "overlays", n) for n in ("battle", "menu", "extra", "supacc"))
    run(["cmake", "-S", os.path.join(adir, "sdk"), "-B", bd, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + tc, "-DCMAKE_BUILD_TYPE=Release",
         "-DPSPRECOMP_OUTPUT_DIR=" + os.path.join(work, "output"), "-DPSPRECOMP_OVERLAY_DIRS=" + ovl], env=env)
    step(5, "Compiling the game for Android (the long step)")
    ninja = re.compile(r"^\[(\d+)/(\d+)\]")

    def prog(line):
        m = ninja.match(line)
        if m and int(m.group(2)):
            tag("PROGRESS", str(100 * int(m.group(1)) // int(m.group(2))))

    run(["cmake", "--build", bd, "-j", str(jobs), "--target", "psprecomp_runtime"], env=env, filt=prog)
    lib = os.path.join(bd, "libmain.so")
    if not os.path.isfile(lib):
        die("the build finished but %s is missing" % lib)
    apk = ab.pack_apk(adir, work, lib, run, say, env)
    tag("DONE", apk)
    say("\nDone: %s\nCopy it to the device with games/dissidia/tools/android_install.py (needs adb), or install the APK by hand and copy the game folder (see docs/android.md)." % apk)


def main():
    global PROGRESS
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("workdir")
    ap.add_argument("--sdk")
    ap.add_argument("--repo", default=os.path.abspath(os.path.join(HERE, "..", "..", "..")))
    ap.add_argument("--game", default=os.path.abspath(os.path.join(HERE, "..")))
    ap.add_argument("--psprecomp")
    ap.add_argument("--toolchain")
    ap.add_argument("--allow-system-compiler", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--progress", action="store_true")
    ap.add_argument("--no-compile", action="store_true", help="stop after the translation steps (analysis, recompile, overlays)")
    ap.add_argument("--android", nargs="?", const="auto", default=None, metavar="DIR",
                    help="build the Android version (an APK) instead of the desktop game; DIR = the release's android/ folder")
    a = ap.parse_args()
    PROGRESS = a.progress

    work = os.path.abspath(a.workdir)
    game = os.path.abspath(a.game)
    repo = os.path.abspath(a.repo)
    disc = os.path.join(work, "disc0", "PSP_GAME")
    if not os.path.isdir(work):
        die("workdir %s does not exist (run setup_from_iso.py first)" % work)
    jobs = a.jobs or min(3, os.cpu_count() or 2)
    if not a.jobs and os.path.isfile("/proc/meminfo"):   # about 1.5 GB per compile job: a phone is killed, not slowed, when it runs out
        try:
            kb = int(re.search(r"MemTotal:\s+(\d+)", open("/proc/meminfo").read()).group(1))
            jobs = max(1, min(jobs, int(kb / 1048576 / 2.2)))
        except (OSError, AttributeError, ValueError):
            pass

    env = dict(os.environ)
    exe = ".exe" if os.name == "nt" else ""
    tc_file = None   # the bundle's CMake toolchain file, when --toolchain is a bundle
    if a.toolchain:
        tc = os.path.abspath(a.toolchain)
        if not os.path.isdir(tc):
            die("toolchain %s does not exist" % tc)
        env["PATH"] = os.pathsep.join([os.path.join(tc, "bin"), os.path.join(tc, "cmake", "bin"), env.get("PATH", "")])
        if os.path.isfile(os.path.join(tc, "toolchain.cmake")):
            tc_file = os.path.join(tc, "toolchain.cmake")
            env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(work, ".zig-cache")   # libc++ etc. are built once per work folder
            env.pop("CC", None)
            env.pop("CXX", None)
        pc = os.path.join(tc, "lib", "pkgconfig")
        if os.path.isdir(pc):
            env["PKG_CONFIG_PATH"] = os.pathsep.join([pc, env.get("PKG_CONFIG_PATH", "")]).rstrip(os.pathsep)
    if a.sdk:
        # ABI guard: the prebuilt core and the code compiled here must come from the same compiler + C++ library.
        sdk_id = os.path.join(os.path.abspath(a.sdk), "toolchain.json")
        if os.path.isfile(sdk_id):
            want = json.load(open(sdk_id))
            if tc_file:
                have = json.load(open(os.path.join(os.path.dirname(tc_file), "toolchain.json")))
                diff = [k for k in ("bundle_version", "platform", "zig_target", "zig") if want.get(k) != have.get(k)]
                if diff:
                    die("the SDK was built with toolchain bundle %s but %s is a different one (differs in %s)" %
                        (json.dumps({k: want.get(k) for k in diff}), a.toolchain, ", ".join(diff)))
            elif not a.allow_system_compiler:
                die("this SDK was built with the release's toolchain bundle (zig %s, %s); pass --toolchain <bundle>" %
                    (want.get("zig"), want.get("zig_target")))

    # ------------------------------------------------------------------ prerequisites
    psprecomp = a.psprecomp or os.path.join(repo, "target", "release", "psprecomp" + exe)
    if not os.path.isfile(psprecomp):
        if not a.psprecomp and shutil.which("cargo", path=env["PATH"]):
            say("Building the psprecomp recompiler (cargo build --release)")
            run(["cargo", "build", "--release"], env=env, cwd=repo)
        if not os.path.isfile(psprecomp):
            die("the recompiler %s was not found (pass --psprecomp, or install Rust to build it)" % psprecomp)
    env["PSPRECOMP_BIN"] = os.path.abspath(psprecomp)   # build_overlay.py runs the recompiler too
    for tool in (() if a.no_compile or a.android else ("cmake",)):
        if not shutil.which(tool, path=env["PATH"]):
            die("%s not found in PATH (a release bundles it; pass --toolchain)" % tool)
    eboot = os.path.join(work, "EBOOT.dec.elf")
    if not os.path.isfile(eboot):
        die("%s missing: run setup_from_iso.py first" % eboot)

    # ------------------------------------------------------------------ 1. analysis
    step(1, "Rebuilding the analysis from your files")
    shipped = None
    for d in (work, os.path.join(game, "analysis"), HERE):
        if all(os.path.isfile(os.path.join(d, n + ".json.gz")) for n in ANALYSES):
            shipped = d
            break
    have_all = all(os.path.isfile(os.path.join(work, n + ".json")) for n in ANALYSES)
    if shipped is None and not have_all:
        die("no analysis*.json.gz found (looked in %s, %s)" % (work, os.path.join(game, "analysis")))
    sources = {
        "analysis": eboot,
        "analysis_battle": os.path.join(disc, "USRDIR", "DATA", "OVL_BATTLE_APP.ELF"),
        "analysis_menu": os.path.join(disc, "USRDIR", "DATA", "OVL_MENU_APP.ELF"),
        "analysis_extra": os.path.join(disc, "USRDIR", "DATA", "OVL_EXTRA_APP.ELF"),
        "analysis_supacc": os.path.join(disc, "USRDIR", "DATA", "MODULE", "LIBSUPPREACC.PRX"),
    }
    if shipped is not None:
        for n in ANALYSES:
            out = os.path.join(work, n + ".json")
            if os.path.isfile(out) and not a.force and os.path.getmtime(out) >= os.path.getmtime(os.path.join(shipped, n + ".json.gz")):
                continue
            run([sys.executable] + ([] if os.environ.get("PYTHONHOME") else ["-I"]) + [os.path.join(HERE, "ship_analysis.py"), "hydrate",
                 os.path.join(shipped, n + ".json.gz"), sources[n], out], env=env)
    for name, lo in (("analysis", 5000), ("analysis_battle", 3000), ("analysis_menu", 3000), ("analysis_extra", 40), ("analysis_supacc", 30)):
        with open(os.path.join(work, name + ".json")) as f:
            n = len(json.load(f)["functions"])
        say("  %-18s %6d functions" % (name, n))
        if n < lo:
            die("%s has only %d functions (expected at least %d): wrong game version?" % (name, n, lo))

    # ------------------------------------------------------------------ 2. recompile + 3. overlays
    stamp = os.path.join(work, ".recompile.stamp")
    fp_inputs = [psprecomp] + [os.path.join(work, n + ".json") for n in ANALYSES] + [os.path.join(game, "game.toml")]
    fp_inputs += sorted(os.path.join(game, f) for f in os.listdir(game) if f.startswith("overlay_") and f.endswith(".toml"))
    fp_inputs.append(os.path.join(HERE, "build_overlay.py"))
    fp = sha256_files(fp_inputs)
    complete = all(os.path.isfile(os.path.join(work, p)) for p in
                   ("output/include/recomp.h", "overlays/battle/dispatch.cpp", "overlays/supacc/dispatch.cpp"))
    old = open(stamp).read().strip() if os.path.isfile(stamp) else ""
    if a.force or old != fp or not complete:
        if os.path.exists(stamp):
            os.remove(stamp)
        step(2, "Translating the game to C++ (recompile)")
        shutil.rmtree(os.path.join(work, "output"), ignore_errors=True)
        renv = dict(env, PSPRECOMP_CROSS_MID="1")
        run([psprecomp, "recompile", os.path.join(work, "analysis.json"), "--config", os.path.join(game, "game.toml"),
             "-o", os.path.join(work, "output")], env=renv)
        rh = os.path.join(work, "output", "include", "recomp.h")
        if not os.path.isfile(rh):
            die("%s was not generated" % rh)
        with open(rh) as f:
            text = f.read()
        if "#include <cmath>" not in text:
            with open(rh, "w") as f:
                f.write("#include <cmath>\n#include <math.h>\n" + text)
        step(3, "Building the overlay sources")
        shutil.rmtree(os.path.join(work, "overlays"), ignore_errors=True)
        os.makedirs(os.path.join(work, "overlays"))
        bo = os.path.join(HERE, "build_overlay.py")
        ana = os.path.join(work, "analysis.json")
        for idx, prefix, name, extra in ((1, "ovl_battle", "battle", []), (2, "ovl_menu", "menu", []), (3, "ovl_extra", "extra", []),
                                         (0, "supacc", "supacc", ["--module"])):
            run([sys.executable, bo, str(idx), prefix, os.path.join(work, "analysis_%s.json" % name), ana,
                 os.path.join(work, "overlays", name), os.path.join(game, "overlay_%s.toml" % name)] + extra, env=env)
            if not os.path.isfile(os.path.join(work, "overlays", name, "dispatch.cpp")):
                die("overlay %s was not generated" % name)
        with open(stamp, "w") as f:
            f.write(fp + "\n")
    else:
        step(2, "Translating the game to C++: up to date, skipping (--force to redo)")
        step(3, "Building the overlay sources: up to date, skipping")

    if a.no_compile:
        tag("DONE", work)
        return
    if a.android:
        android_build(a, work, env, jobs, exe)
        return

    # ------------------------------------------------------------------ 4. configure + build
    step(4, "Configuring the build")
    build = os.path.join(work, "build")
    src = os.path.abspath(a.sdk) if a.sdk else os.path.join(repo, "runtime")
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.isfile(cache):   # configured with another compiler (e.g. the system one before the bundle was used): start over
        c = open(cache).read()
        m = re.search(r"^CMAKE_TOOLCHAIN_FILE:[A-Z]+=(.*)$", c, re.M)
        if (m.group(1) if m else None) != tc_file:
            say("Build dir was configured with a different toolchain; reconfiguring from scratch")
            shutil.rmtree(build, ignore_errors=True)
    if os.path.isfile(cache):   # configured from another source dir (moved or re-cloned): reconfigure from this one
        m = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", open(cache).read(), re.M)
        if m and os.path.realpath(m.group(1)) != os.path.realpath(src):
            say("Build dir was configured from %s; reconfiguring from %s" % (m.group(1), src))
            os.remove(cache)
            shutil.rmtree(os.path.join(build, "CMakeFiles"), ignore_errors=True)
    ovl = ";".join(os.path.join(work, "overlays", n) for n in ("battle", "menu", "extra", "supacc"))
    cfg = ["cmake", "-S", src, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DPSPRECOMP_OUTPUT_DIR=" + os.path.join(work, "output"),
           "-DPSPRECOMP_OVERLAY_DIRS=" + ovl]
    if not a.sdk:
        cfg.append("-DPSPRECOMP_GAME=dissidia")
    if tc_file:
        cfg += ["-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + tc_file, "-DCMAKE_EXE_LINKER_FLAGS=-s"]   # -s: no debug info (212 -> ~45 MB)
    elif shutil.which("ninja", path=env["PATH"]):
        cfg += ["-G", "Ninja"] if not os.path.isfile(cache) else []
    if shutil.which("ccache", path=env["PATH"]):
        cfg.append("-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
    run(cfg, env=env)

    step(5, "Compiling the game (the long step; about 1.5 GB RAM per job)")
    pct = re.compile(r"^\[\s*(\d+)%\]")
    ninja = re.compile(r"^\[(\d+)/(\d+)\]")

    def prog(line):
        m = pct.match(line)
        if m:
            tag("PROGRESS", m.group(1))
            return
        m = ninja.match(line)
        if m and int(m.group(2)):
            tag("PROGRESS", str(100 * int(m.group(1)) // int(m.group(2))))

    run(["cmake", "--build", build, "-j", str(jobs), "--target", "psprecomp_runtime"], env=env, filt=prog)
    binary = os.path.join(build, "psprecomp_runtime" + exe)
    if not os.path.isfile(binary):
        die("the build finished but %s is missing" % binary)
    tag("DONE", binary)
    say("\nDone: %s\nRun it: %s" % (binary, os.path.join(game, "tools", "run.sh") + " " + work))


if __name__ == "__main__":
    main()
