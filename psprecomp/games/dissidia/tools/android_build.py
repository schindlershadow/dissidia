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
import hashlib
import json
import os
import shutil
import stat
import sys
import urllib.request
import zipfile


def host_key():
    return "windows" if os.name == "nt" else "linux"


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
    with zipfile.ZipFile(zp) as z:
        names = [i for i in z.infolist() if i.filename.split("/", 1)[-1].startswith((prebuilt, "build/", "meta/")) or
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
    strip = os.path.join(adir, "ndk", "toolchains", "llvm", "prebuilt", v["ndk"]["files"][host_key()]["prebuilt"], "bin", "llvm-strip" + exe)
    stripped = os.path.join(out_dir, "libmain.so")
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
