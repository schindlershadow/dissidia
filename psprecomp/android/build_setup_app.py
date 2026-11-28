#!/usr/bin/env python3
"""Build libsetupcore.so of the Android setup app: the install engine of setup/ (plans, processes, progress, payload; setup/android_engine.cpp
is its JNI face) for the app's Java UI (android/setup/java). Built with zig and the NDK sysroot.

  build_setup_app.py --zig ZIG --ndk NDK_ROOT --out libsetupcore.so [--jobs N]

The same compiler and flags the phone uses to build the game (games/dissidia/tools/android_build.py device_build): zig as clang + lld against
the NDK sysroot, linked with c++_static. No SDL: the pages are Android views. Standard library only.
"""
import argparse, concurrent.futures, glob, hashlib, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SYSROOT = "toolchains/llvm/prebuilt/linux-x86_64/sysroot"
CLANG_RT = "toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux"
API = 29


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--zig", required=True); ap.add_argument("--ndk", required=True)
    ap.add_argument("--deps", help="(ignored; the engine needs no SDL)")
    ap.add_argument("--out", required=True); ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()
    sysroot = os.path.join(a.ndk, SYSROOT)
    lib = sysroot + "/usr/lib/aarch64-linux-android"
    rt = os.path.join(a.ndk, CLANG_RT)
    work = tempfile.mkdtemp(prefix="setupapp-")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(work, "zig-cache"), HOME=work)
    cxx = [REPO + "/setup/" + f for f in ("install.cpp", "extras.cpp", "proc.cpp", "payload.cpp", "android_engine.cpp")]
    c = [REPO + "/setup/third_party/miniz.c"]
    # Report an issue: the game's psp_report.cpp (scrub, zip, system facts) without its upload code (Java posts over HTTPS); zlib is the system's
    rep = REPO + "/runtime/src/psp_report.cpp"
    common = ["-target", "aarch64-linux-android", "-DANDROID", "-U__ANDROID_API__", "-D__ANDROID_API__=%d" % API, "-fPIC", "-O2", "-ffp-contract=off",
              "-fdata-sections", "-ffunction-sections", "-funwind-tables", "-fstack-protector-strong", "-Wno-everything",
              "-isystem", sysroot + "/usr/include/aarch64-linux-android", "-isystem", sysroot + "/usr/include",
              "-I" + REPO + "/setup", "-I" + REPO + "/runtime/include", "-I" + REPO + "/runtime/src"]
    cxxflags = ["-std=gnu++17", "-nostdinc++", "-I" + sysroot + "/usr/include/c++/v1"]
    objs = []

    def build(src):
        o = os.path.join(work, hashlib.sha1(src.encode()).hexdigest()[:8] + "_" + os.path.basename(src) + ".o")
        is_c = src.endswith(".c")
        cmd = [a.zig, "cc" if is_c else "c++"] + common + ([] if is_c else cxxflags) + \
              (["-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES", "-DMINIZ_NO_ARCHIVE_WRITING_APIS"] if "miniz" in src else []) + \
              (["-DPSP_REPORT_STANDALONE", "-DPSP_REPORT_NO_HTTP"] if src == rep else []) + ["-c", src, "-o", o]
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            sys.exit("compile failed: %s\n%s" % (src, r.stdout[-3000:]))
        return o

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        objs = list(ex.map(build, cxx + [rep] + c))
    cl = lib + "/%d" % API
    cmd = [a.zig, "ld.lld", "-m", "aarch64linux", "-shared", "-z", "noexecstack", "-z", "relro", "-z", "now", "-z", "max-page-size=16384", "--hash-style=gnu",
           "--eh-frame-hdr", "--build-id=sha1", "--gc-sections", "--threads=1", "-s", "--no-undefined", "-soname", os.path.basename(a.out), "-o", a.out,
           cl + "/crtbegin_so.o"] + objs + ["-L" + cl, "-L" + lib, "-llog", "-lz",
           "-lm", lib + "/libc++_static.a", lib + "/libc++abi.a", rt + "/aarch64/libunwind.a", rt + "/aarch64/libatomic.a", "-ldl", "-lc",
           rt + "/libclang_rt.builtins-aarch64-android.a", cl + "/crtend_so.o"]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.exit("link failed:\n" + r.stdout[-3000:])
    print("built", a.out, os.path.getsize(a.out))


if __name__ == "__main__":
    main()
