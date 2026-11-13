#!/usr/bin/env python3
"""Build libmain.so of the Android setup app (the wizard in setup/, as a shared library for SDL's Java activity) with zig and the NDK sysroot.

  build_setup_app.py --zig ZIG --ndk NDK_ROOT --deps DEPS_DIR --out libmain.so [--jobs N]

DEPS_DIR is the Android part's deps/deps (SDL2 headers + libSDL2.so, FreeType). The same compiler and flags the phone uses to build the game
(games/dissidia/tools/android_build.py device_build): zig as clang + lld against the NDK sysroot, linked with c++_static. Standard library only.
"""
import argparse, concurrent.futures, glob, hashlib, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
SYSROOT = "toolchains/llvm/prebuilt/linux-x86_64/sysroot"
CLANG_RT = "toolchains/llvm/prebuilt/linux-x86_64/lib/clang/18/lib/linux"
API = 29


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--zig", required=True); ap.add_argument("--ndk", required=True); ap.add_argument("--deps", required=True)
    ap.add_argument("--out", required=True); ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    a = ap.parse_args()
    sysroot = os.path.join(a.ndk, SYSROOT)
    lib = sysroot + "/usr/lib/aarch64-linux-android"
    rt = os.path.join(a.ndk, CLANG_RT)
    work = tempfile.mkdtemp(prefix="setupapp-")
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(work, "zig-cache"), HOME=work)
    # the menu fonts, embedded as C++ (the same cmake script setup/CMakeLists.txt uses)
    fonts = os.path.join(work, "psp_ui_fonts.cpp")
    subprocess.check_call(["cmake", "-DFONT_DIR=" + REPO + "/runtime/assets/fonts", "-DOUT=" + fonts, "-P", REPO + "/runtime/assets/embed_fonts.cmake"])
    rtsrc = REPO + "/runtime/src/hle/"
    cxx = [REPO + "/setup/" + f for f in ("setup_main.cpp", "install.cpp", "proc.cpp", "payload.cpp", "android.cpp")] + \
          [rtsrc + f for f in ("psp_savedata_dialog.cpp", "psp_savemgr.cpp", "psp_savedata_crypto.cpp")] + [fonts]
    c = [REPO + "/setup/third_party/miniz.c"]
    common = ["-target", "aarch64-linux-android", "-DANDROID", "-U__ANDROID_API__", "-D__ANDROID_API__=%d" % API, "-fPIC", "-O2",
              "-fdata-sections", "-ffunction-sections", "-funwind-tables", "-fstack-protector-strong", "-Wno-everything",
              "-isystem", sysroot + "/usr/include/aarch64-linux-android", "-isystem", sysroot + "/usr/include",
              "-I" + REPO + "/runtime/include", "-I" + rtsrc, "-isystem", a.deps + "/include/SDL2", "-isystem", a.deps + "/include/freetype2",
              "-isystem", a.deps + "/include"]
    cxxflags = ["-std=gnu++17", "-nostdinc++", "-I" + sysroot + "/usr/include/c++/v1"]
    objs = []

    def build(src):
        o = os.path.join(work, hashlib.sha1(src.encode()).hexdigest()[:8] + "_" + os.path.basename(src) + ".o")
        is_c = src.endswith(".c")
        cmd = [a.zig, "cc" if is_c else "c++"] + common + ([] if is_c else cxxflags) + \
              (["-DMINIZ_NO_ZLIB_COMPATIBLE_NAMES", "-DMINIZ_NO_ARCHIVE_WRITING_APIS"] if "miniz" in src else []) + ["-c", src, "-o", o]
        r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            sys.exit("compile failed: %s\n%s" % (src, r.stdout[-3000:]))
        return o

    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        objs = list(ex.map(build, cxx + c))
    d = a.deps + "/lib"
    cl = lib + "/%d" % API
    cmd = [a.zig, "ld.lld", "-m", "aarch64linux", "-shared", "-z", "noexecstack", "-z", "relro", "-z", "now", "-z", "max-page-size=16384", "--hash-style=gnu",
           "--eh-frame-hdr", "--build-id=sha1", "--gc-sections", "--threads=1", "-s", "--no-undefined", "-soname", "libmain.so", "-o", a.out,
           cl + "/crtbegin_so.o"] + objs + [d + "/libSDL2.so", d + "/libfreetype.a", cl + "/libz.so", "-L" + cl, "-L" + lib, "-llog", "-landroid", "-lGLESv3",
           "-lm", lib + "/libc++_static.a", lib + "/libc++abi.a", rt + "/aarch64/libunwind.a", rt + "/aarch64/libatomic.a", "-ldl", "-lc",
           rt + "/libclang_rt.builtins-aarch64-android.a", cl + "/crtend_so.o"]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode:
        sys.exit("link failed:\n" + r.stdout[-3000:])
    print("built", a.out, os.path.getsize(a.out))


if __name__ == "__main__":
    main()
