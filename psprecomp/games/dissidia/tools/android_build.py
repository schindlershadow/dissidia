"""Android build for psp_build.py (--android): the player's machine compiles the generated game with Google's NDK compiler against the
release's prebuilt Android core and libraries, then packs and signs an APK in pure Python. Python 3 standard library only.

Release layout (<release>/android, built by release/build_android_sdk.py):
  versions.json   pinned NDK / API / package         android.json  what the prebuilt parts were built with (checked)
  sdk/            the Android runtime core + link project (CMakeLists.txt, lib/libpsprecomp_core.a, include/, src/main.cpp)
  deps/           deps.cmake + deps/{include,lib}: SDL2 (shared), FreeType, FFmpeg (ATRAC3/3+ and H.264 decoders)
  apk/            skeleton.apk (manifest + classes.dex), apk_pack.py, debug_key.json
  libc++_shared.so
  ndk/            downloaded on first use (about 0.7 GB download, 2 GB on disk): Google's Android NDK, Apache-2.0 + component licenses
"""
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import stat
import sys
import urllib.request
import zipfile


def is_device():
    """True when this runs ON an Android phone (the setup app's bundled Python): there the game is compiled by the bundled zig, see device_build()."""
    return os.path.exists("/system/bin/app_process") or bool(os.environ.get("PSPRECOMP_ANDROID_DEVICE"))


def host_key():
    return "windows" if os.name == "nt" else "linux"


SYSROOT_REL = "toolchains/llvm/prebuilt/linux-x86_64/sysroot"   # the NDK's target files (headers, libc++, stub libraries) are the same for every host


def load_versions(adir):
    return json.load(open(os.path.join(adir, "versions.json")))


def ndk_ready(adir):
    v = load_versions(adir)
    marker = os.path.join(adir, "ndk", ".psprecomp-ndk")
    return os.path.isfile(marker) and open(marker).read().strip() == v["ndk"]["version"]


def fetch_ndk(adir, say, tag, die):
    """Download (once) and unpack the part of the NDK that is needed: the clang toolchain with its sysroot and the CMake build files."""
    v = load_versions(adir)
    ent = v["ndk"]["files"][host_key()]
    dest = os.path.join(adir, "ndk")
    if ndk_ready(adir):
        return dest
    os.makedirs(os.path.join(adir, ".download"), exist_ok=True)
    zp = os.path.join(adir, ".download", ent["url"].rsplit("/", 1)[1])
    if not (os.path.isfile(zp) and os.path.getsize(zp) == ent["size"]):
        say("Downloading Google's Android NDK %s (%d MB, one time) from %s" % (v["ndk"]["name"], ent["size"] // 1000000, ent["url"]))
        req = urllib.request.Request(ent["url"], headers={"User-Agent": "psprecomp-android/1"})
        part = zp + ".part"
        done = 0
        last = -1
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                pct = 100 * done // ent["size"]
                if pct != last:
                    last = pct
                    tag("PROGRESS", str(min(pct, 99)))
        os.replace(part, zp)
    h = hashlib.sha1()
    with open(zp, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != ent["sha1"]:
        os.remove(zp)
        die("the downloaded NDK does not match its published checksum (deleted; try again)")
    say("Unpacking the NDK")
    shutil.rmtree(dest, ignore_errors=True)
    prebuilt = "toolchains/llvm/prebuilt/" + ent["prebuilt"] + "/"
    keep = (prebuilt, "build/", "meta/")
    with zipfile.ZipFile(zp) as z:
        names = [i for i in z.infolist() if i.filename.split("/", 1)[-1].startswith(keep) or
                 i.filename.split("/", 1)[-1] == "source.properties"]
        for k, info in enumerate(names):
            rel = info.filename.split("/", 1)[1]
            if not rel or info.is_dir():
                continue
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            mode = info.external_attr >> 16
            if stat.S_ISLNK(mode) and os.name != "nt":
                target = z.read(info).decode()
                if os.path.lexists(out):
                    os.remove(out)
                os.symlink(target, out)
                continue
            with z.open(info) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
            if mode & 0o111 and os.name != "nt":
                os.chmod(out, 0o755)
            if k % 500 == 0:
                tag("PROGRESS", str(99))
    with open(os.path.join(dest, ".psprecomp-ndk"), "w") as f:
        f.write(v["ndk"]["version"] + "\n")
    try:
        os.remove(zp)
    except OSError:
        pass
    return dest


def check_prebuilt(adir, die):
    """The prebuilt core/libraries and the NDK that compiles the game must be the same release of the NDK."""
    v = load_versions(adir)
    ap = os.path.join(adir, "android.json")
    if not os.path.isfile(ap):
        die("this release has no Android build (android/android.json missing)")
    built = json.load(open(ap))
    if built.get("ndk") != v["ndk"]["version"] or built.get("api") != v["api"]:
        die("the release's Android core was built with NDK %s / API %s but this script pins NDK %s / API %s" %
            (built.get("ndk"), built.get("api"), v["ndk"]["version"], v["api"]))


def write_toolchain(adir, work):
    v = load_versions(adir)
    bd = os.path.join(work, "android", "build")
    os.makedirs(bd, exist_ok=True)
    tc = os.path.join(bd, "toolchain.cmake")
    ndk = os.path.join(adir, "ndk").replace("\\", "/")
    with open(tc, "w") as f:
        f.write('set(ANDROID_ABI %s)\nset(ANDROID_PLATFORM android-%d)\ninclude("%s/build/cmake/android.toolchain.cmake")\n'
                'set(PSPRECOMP_DEPS_CMAKE "%s/deps/deps.cmake")\n' % (v["abi"], v["api"], ndk, adir.replace("\\", "/")))
    return bd, tc


def pack_apk(adir, work, libmain, run, say, env):
    v = load_versions(adir)
    out_dir = os.path.join(work, "android")
    os.makedirs(out_dir, exist_ok=True)
    apk = os.path.join(out_dir, "dissidia.apk")
    exe = ".exe" if os.name == "nt" else ""
    stripped = os.path.join(out_dir, "libmain.so")
    if is_device():   # device_build() already links with -s
        shutil.copyfile(libmain, stripped)
    else:
        strip = os.path.join(adir, "ndk", "toolchains", "llvm", "prebuilt", v["ndk"]["files"][host_key()]["prebuilt"], "bin", "llvm-strip" + exe)
        run([strip, "-o", stripped, libmain], env=env)
    abi = v["abi"]
    run([sys.executable, os.path.join(adir, "apk", "apk_pack.py"), "--skeleton", os.path.join(adir, "apk", "skeleton.apk"),
         "--key", os.path.join(adir, "apk", "debug_key.json"),
         "--lib", "lib/%s/libmain.so=%s" % (abi, stripped),
         "--lib", "lib/%s/libSDL2.so=%s" % (abi, os.path.join(adir, "deps", "deps", "lib", "libSDL2.so")),
         "--lib", "lib/%s/libc++_shared.so=%s" % (abi, os.path.join(adir, "libc++_shared.so"))]
        # libadrenotools' hook libraries (custom GPU drivers): they must sit in nativeLibraryDir, next to libmain.so
        + sum((["--lib", "lib/%s/%s=%s" % (abi, n, os.path.join(adir, "deps", "deps", "lib", n))]
               for n in ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
               if os.path.isfile(os.path.join(adir, "deps", "deps", "lib", n))), [])
        + (["--lib", "assets/shaders.pack=" + os.path.join(adir, "shaders.pack")] if os.path.isfile(os.path.join(adir, "shaders.pack")) else [])   # shaders compiled at start ("Preparing graphics")
        + ["-o", apk], env=env)
    return apk


# --------------------------------------------------------------------------------------------------------------- on the phone

def device_build(adir, work, jobs, say, tag, die):
    """Compile + link the generated game ON the phone with the zig that ships in the setup app (zig is clang + lld in one static binary).

    The prebuilt core (adir/sdk) and libraries (adir/deps) come from the NDK, so the game is compiled against the NDK's headers and
    libc++ (`__ndk1`) exactly like the PC build; only the compiler front end differs. Findings (RP6, 2026-11-13):
      - the NDK's libc++ headers must come with -I (zig puts its own include dir before -isystem ones);
      - `zig cc` will not link for Android ("unable to provide libc"): link with `zig ld.lld` and the NDK's crt files / libraries;
      - lld must run with --threads=1 (the parallel linker crashes on Android).
    Layout of adir (the setup app's payload): ndk/<SYSROOT_REL>, ndk/clang-rt/{libclang_rt.builtins-aarch64-android.a,libunwind.a,libatomic.a},
    zig-lib/ (zig's lib/ include + std), sdk/, deps/. The zig binary: $PSPRECOMP_ZIG (the app's libzig.so) or adir/bin/zig."""
    v = load_versions(adir)
    zig = os.environ.get("PSPRECOMP_ZIG") or os.path.join(adir, "bin", "zig")
    if not os.path.isfile(zig):
        die("the compiler (zig) was not found: %s" % zig)
    api = v["api"]
    sysroot = os.path.join(adir, "ndk", SYSROOT_REL)
    lib = sysroot + "/usr/lib/aarch64-linux-android"
    rt = os.path.join(adir, "ndk", "clang-rt")
    for need in (sysroot + "/usr/include/c++/v1", lib + "/%d/crtbegin_so.o" % api, rt + "/libclang_rt.builtins-aarch64-android.a",
                 os.path.join(adir, "sdk", "lib", "libpsprecomp_core.a")):
        if not os.path.exists(need):
            die("incomplete Android part: %s is missing" % need)
    base = os.path.join(work, "android")
    objdir = os.path.join(base, "obj")
    os.makedirs(objdir, exist_ok=True)
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(base, "zig-cache"), HOME=base,
               ZIG_LIB_DIR=os.path.join(adir, "zig-lib"))
    deps = os.path.join(adir, "deps", "deps")
    out = os.path.join(work, "output")
    ovl = os.path.join(work, "overlays")
    oi = ["-I" + out + "/include", "-I" + out]
    # (source, include flags): the same sources the SDK's CMakeLists compiles
    srcs = []
    import glob
    for f in sorted(glob.glob(out + "/generated/batch_*.cpp")) + [os.path.join(out, n) for n in
            ("dispatch.cpp", "data_sections.cpp", "init_array.cpp", "mid_entries.cpp", "syscall_table.cpp")]:
        srcs.append((f, oi))
    for name in ("battle", "menu", "extra", "supacc"):
        d = os.path.join(ovl, name)
        inc = ["-I" + d] + oi
        files = sorted(glob.glob(d + "/generated/batch_*.cpp")) + [d + "/dispatch.cpp", d + "/mid_entries.cpp"]
        meta = os.path.join(d, "overlay.txt")
        if os.path.isfile(meta) and open(meta).read().startswith("0 "):
            files += [d + "/data_sections.cpp", d + "/syscall_table.cpp"]
        srcs += [(f, inc) for f in files]
    main_cpp = os.path.join(adir, "sdk", "src", "main.cpp")
    srcs.append((main_cpp, ["-I" + os.path.join(adir, "sdk", "include")] + oi +
                 ["-isystem", deps + "/include/SDL2", "-isystem", deps + "/include/freetype2", "-isystem", deps + "/include"]))
    flags = ["-target", "aarch64-linux-android", "-nostdinc++", "-I" + sysroot + "/usr/include/c++/v1",
             "-isystem", sysroot + "/usr/include/aarch64-linux-android", "-isystem", sysroot + "/usr/include",
             "-DANDROID", "-U__ANDROID_API__", "-D__ANDROID_API__=%d" % api, "-fdata-sections", "-ffunction-sections", "-funwind-tables",
             "-fstack-protector-strong", "-D_FORTIFY_SOURCE=2", "-std=gnu++17", "-fPIC", "-O2", "-fno-lto", "-fno-strict-aliasing", "-Wno-everything"]
    stamp = hashlib.sha256((" ".join(flags) + zig).encode()).hexdigest()[:12]

    def obj_of(src):
        return os.path.join(objdir, hashlib.sha1(src.encode()).hexdigest()[:10] + "_" + os.path.basename(src) + "." + stamp + ".o")

    todo = [(s, inc) for s, inc in srcs if not (os.path.isfile(obj_of(s)) and os.path.getmtime(obj_of(s)) >= os.path.getmtime(s))]
    say("Compiling %d of %d files with zig (%d at a time)" % (len(todo), len(srcs), jobs))
    done = [0]
    errors = []

    def compile_one(item):
        src, inc = item
        o = obj_of(src)
        part = o + ".part"
        r = subprocess.run([zig, "c++"] + flags + inc + ["-c", src, "-o", part], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace")
        if r.returncode != 0:
            errors.append("%s\n%s" % (src, r.stdout[-1500:]))
            return
        os.replace(part, o)
        done[0] += 1
        tag("PROGRESS", str(100 * done[0] // max(1, len(todo))))
        say("[%d/%d] %s" % (done[0], len(todo), os.path.basename(src)))

    if todo:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            list(ex.map(compile_one, todo))
    if errors:
        die("compiling failed:\n" + errors[0])
    say("Linking libmain.so")
    tag("PROGRESS", "99")
    objs = [obj_of(s) for s, _ in srcs if s != main_cpp]
    cl = lib + "/%d" % api
    dl = deps + "/lib"
    libmain = os.path.join(base, "build", "libmain.so")
    os.makedirs(os.path.dirname(libmain), exist_ok=True)
    cmd = [zig, "ld.lld", "-m", "aarch64linux", "-shared", "-z", "noexecstack", "-z", "relro", "-z", "now", "-z", "max-page-size=16384",
           "--hash-style=gnu", "--eh-frame-hdr", "--build-id=sha1", "--gc-sections", "--threads=1", "-s", "--no-rosegment", "--no-undefined",
           "--fatal-warnings", "-soname", "libmain.so", "-o", libmain, cl + "/crtbegin_so.o", obj_of(main_cpp),
           "--start-group", os.path.join(adir, "sdk", "lib", "libpsprecomp_core.a")] + objs + ["--end-group",
           dl + "/libSDL2.so", dl + "/libfreetype.a", cl + "/libz.so", "-L" + cl, "-L" + lib, "-lGLESv3", "-llog", "-landroid",
           dl + "/libavcodec.a", dl + "/libavutil.a", "-lm", lib + "/libc++_static.a", lib + "/libc++abi.a",
           rt + "/libunwind.a", rt + "/libatomic.a", "-lm", "-ldl", "-lc", rt + "/libclang_rt.builtins-aarch64-android.a", cl + "/crtend_so.o"]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode != 0:
        die("linking failed:\n" + r.stdout[-2000:])
    return libmain
