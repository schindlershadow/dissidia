#!/usr/bin/env python3
"""Build the portable toolchain bundle a release ships (and that CI builds the prebuilt runtime core with).

  build_bundle.py <platform> --out DIR [--cache DIR] [--work DIR] [--jobs N] [--print-hashes]

  platform   linux-x86_64 | windows-x86_64   (see versions.json; Windows is cross-built from a Linux host)
  --out      the bundle directory to create (it is relocatable: copy or unpack it anywhere)
  --cache    download cache (default: <out>.cache); every file is verified against the sha256 pinned in versions.json
  --work     scratch directory for source trees and build dirs (default: <out>.work; deleted on success unless --keep)

Why this exists: the runtime core (libpsprecomp_core.a, built once per release) and the generated game code (compiled on the
player's machine) are linked into one program, so both must come from the same compiler and C++ library (libstdc++ and
libc++ do not link together). The bundle is that one compiler: zig 0.15.2 (clang + lld + libc++ + libc headers/stubs for any
glibc/mingw target), plus the libraries the runtime links, built with it.  Layout:

  zig/               the zig distribution                 bin/zig-cc zig-c++ zig-ar zig-ranlib   launchers (zigwrap.c)
  cmake/  bin/ninja  pinned cmake and ninja               deps/{include,lib}   SDL2, FreeType, zlib, libavcodec, glslang (static), Vulkan headers
  toolchain.cmake    CMake toolchain file: compilers, deps, ABI-relevant flags (pass -DCMAKE_TOOLCHAIN_FILE=...)
  deps.cmake         defines the PkgConfig::SDL2 / FREETYPE / AVCODEC and ZLIB::ZLIB targets the runtime CMake uses
  bin/pspdecrypt[.exe]  EBOOT.BIN decryptor for the installer (John-K/pspdecrypt, GPL-3.0; license copy: bin/pspdecrypt-LICENSE.txt),
                     static, with OpenSSL libcrypto (SHA-256, DES only) built from source and linked in
  toolchain.json     what the bundle is (bundle_version, zig target, component versions): the ABI identity of a release

Python 3 standard library only. Needs on the build host (CI): bash, make, tar, and for Linux SDL2 the X11/Wayland/ALSA/...
development headers (SDL loads those libraries at run time; only their headers are needed here).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = "linux-x86_64"


def log(msg):
    print("[bundle] " + msg, flush=True)


def die(msg):
    print("[bundle] ERROR: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(entry, cache, print_only=False):
    """Download entry['url'] into the cache and verify entry['sha256']; returns the local path."""
    os.makedirs(cache, exist_ok=True)
    dest = os.path.join(cache, entry["url"].rsplit("/", 1)[1])
    if not os.path.isfile(dest):
        log("downloading " + entry["url"])
        for attempt in range(4):
            try:
                req = urllib.request.Request(entry["url"], headers={"User-Agent": "psprecomp-bundle/1"})
                with urllib.request.urlopen(req, timeout=60) as r, open(dest + ".part", "wb") as f:
                    shutil.copyfileobj(r, f)
                os.replace(dest + ".part", dest)
                break
            except Exception as e:  # noqa: BLE001
                log("  failed (%s)%s" % (e, "; retrying" if attempt < 3 else ""))
                time.sleep(2 << attempt)
        else:
            die("cannot download " + entry["url"])
    got = sha256(dest)
    if print_only:
        print("%s  %s%s" % (got, os.path.basename(dest), "" if got == entry["sha256"] else "   <-- differs from versions.json"))
    elif got != entry["sha256"]:
        os.remove(dest)
        die("sha256 mismatch for %s: expected %s, got %s (file deleted)" % (dest, entry["sha256"], got))
    return dest


def extract(archive, dest, strip=1):
    """Extract a tar.* / zip into dest (created fresh), dropping `strip` leading path components; keeps exec bits."""
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)

    def target(name):
        parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
        if ".." in parts:
            die("unsafe path in archive: " + name)
        parts = parts[strip:]
        return os.path.join(dest, *parts) if parts else None

    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                t = target(info.filename)
                if t is None:
                    continue
                if info.is_dir():
                    os.makedirs(t, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(t), exist_ok=True)
                with z.open(info) as src, open(t, "wb") as out:
                    shutil.copyfileobj(src, out)
                mode = (info.external_attr >> 16) & 0o777
                os.chmod(t, mode if mode else 0o644)
    else:
        with tarfile.open(archive) as tf:
            for m in tf:
                t = target(m.name)
                if t is None:
                    continue
                if m.isdir():
                    os.makedirs(t, exist_ok=True)
                elif m.issym() or m.islnk():
                    if m.issym():
                        if os.path.lexists(t):
                            os.remove(t)
                        os.makedirs(os.path.dirname(t), exist_ok=True)
                        os.symlink(m.linkname, t)
                    else:
                        src = target(m.linkname)
                        os.makedirs(os.path.dirname(t), exist_ok=True)
                        shutil.copy2(src, t)
                elif m.isfile():
                    os.makedirs(os.path.dirname(t), exist_ok=True)
                    with tf.extractfile(m) as src, open(t, "wb") as out:
                        shutil.copyfileobj(src, out)
                    os.chmod(t, m.mode & 0o777)


def run(cmd, env=None, cwd=None):
    log("$ " + " ".join(cmd))
    r = subprocess.run(cmd, env=env, cwd=cwd)
    if r.returncode != 0:
        die("%s failed (exit %d)" % (os.path.basename(cmd[0]), r.returncode))


def pc_private_libs(pc):
    """The -l flags of a .pc file's Libs.private line (what a static link of that library needs besides itself)."""
    libs = []
    if os.path.isfile(pc):
        for line in open(pc, encoding="utf-8"):
            if line.startswith(("Libs.private:", "Libs:")):   # mingw's sdl2.pc lists the system libraries under plain Libs
                libs += [t[2:] for t in line.split(":", 1)[1].split() if t.startswith("-l")]
    return libs


def build_wrappers(host_zig, outbin, zig_target, exe, self_target=None):
    """Compile zigwrap.c once with the host zig and copy it under each tool name."""
    os.makedirs(outbin, exist_ok=True)
    first = os.path.join(outbin, "zig-cc" + exe)
    cmd = [host_zig, "cc", "-O2", "-DZW_TARGET=\"%s\"" % zig_target, os.path.join(HERE, "zigwrap.c"), "-o", first]
    if self_target:   # the launcher itself runs on the bundle's platform, not on the build host
        cmd[2:2] = ["-target", self_target]
    run(cmd)
    for name in ("zig-c++", "zig-ar", "zig-ranlib", "zig-rc"):
        shutil.copy2(first, os.path.join(outbin, name + exe))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("platform", choices=["linux-x86_64", "windows-x86_64"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache")
    ap.add_argument("--work")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--print-hashes", action="store_true", help="download everything and print the sha256 of each file, then stop")
    ap.add_argument("--keep", action="store_true", help="keep the scratch directory")
    ap.add_argument("--skip-ffmpeg", action="store_true", help="(development) leave libavcodec out")
    a = ap.parse_args()

    if sys.platform != "linux":
        die("the bundle is built on a Linux host (Windows bundles are cross-built)")
    ver = json.load(open(os.path.join(HERE, "versions.json")))
    plat = a.platform
    pinfo = ver["platforms"][plat]
    out = os.path.abspath(a.out)
    cache = os.path.abspath(a.cache or out + ".cache")
    work = os.path.abspath(a.work or out + ".work")
    exe = pinfo["exe"]
    cross = plat != HOST

    # ---------------------------------------------------------------- 1. downloads
    files = {}
    for comp in ("zig", "cmake", "ninja"):
        files[comp] = fetch(ver[comp]["files"][plat], cache, a.print_hashes)
        if cross:
            files[comp + "-host"] = fetch(ver[comp]["files"][HOST], cache, a.print_hashes)
    for name, ent in ver["sources"].items():
        files[name] = fetch(ent, cache, a.print_hashes)
    if a.print_hashes:
        return

    # ---------------------------------------------------------------- 2. the bundle's own tools
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "bin"))
    log("unpacking zig / cmake / ninja")
    extract(files["zig"], os.path.join(out, "zig"))
    extract(files["cmake"], os.path.join(out, "cmake"))
    extract(files["ninja"], os.path.join(out, "bin"), strip=0)
    if os.path.isdir(work):
        shutil.rmtree(work)
    os.makedirs(work)
    if cross:   # tools that RUN on this host while building the Windows bundle
        host = os.path.join(work, "host")
        extract(files["zig-host"], os.path.join(host, "zig"))
        extract(files["cmake-host"], os.path.join(host, "cmake"))
        os.makedirs(os.path.join(host, "bin"))
        extract(files["ninja-host"], os.path.join(host, "bin"), strip=0)
    else:
        host = out
    host_zig = os.path.join(host, "zig", "zig")
    # launchers for the bundle (run on the bundle's platform) and, when cross-building, for this host
    build_wrappers(host_zig, os.path.join(out, "bin"), pinfo["zig_target"], exe, self_target=pinfo["zig_target"] if cross else None)
    if cross:
        build_wrappers(host_zig, os.path.join(host, "bin"), pinfo["zig_target"], "")

    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([os.path.join(host, "bin"), os.path.join(host, "cmake", "bin"), env["PATH"]])
    env["ZIG_GLOBAL_CACHE_DIR"] = os.path.join(work, "zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"] = os.path.join(work, "zig-cache-local")
    hb = os.path.join(host, "bin")

    # internal toolchain file for building the dependencies (the bundle's toolchain.cmake is written at the end)
    tcf = os.path.join(work, "deps_toolchain.cmake")
    with open(tcf, "w") as f:
        if cross:
            f.write("set(CMAKE_SYSTEM_NAME %s)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n" % pinfo["cmake_system"])
        f.write('set(CMAKE_C_COMPILER "%s/zig-cc")\nset(CMAKE_CXX_COMPILER "%s/zig-c++")\n' % (hb, hb))
        f.write('set(CMAKE_AR "%s/zig-ar" CACHE FILEPATH "")\nset(CMAKE_RANLIB "%s/zig-ranlib" CACHE FILEPATH "")\n' % (hb, hb))
        if plat.startswith("windows"):
            f.write('set(CMAKE_RC_COMPILER "%s/zig-rc")\n' % hb)
        f.write('set(CMAKE_MAKE_PROGRAM "%s/ninja" CACHE FILEPATH "")\n' % hb)
        f.write('set(CMAKE_C_FLAGS_INIT "-fno-sanitize=undefined")\nset(CMAKE_CXX_FLAGS_INIT "-fno-sanitize=undefined")\n')
        f.write("set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n")

    deps = os.path.join(out, "deps")
    os.makedirs(deps)
    j = str(a.jobs)

    def cmake_build(name, srcdir, opts):
        bd = os.path.join(work, "build-" + name)
        run(["cmake", "-S", srcdir, "-B", bd, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + tcf,
             "-DCMAKE_INSTALL_PREFIX=" + deps, "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"] + opts, env=env)
        run(["cmake", "--build", bd, "-j", j], env=env)
        run(["cmake", "--install", bd], env=env)

    def src(name, strip=1):
        d = os.path.join(work, "src-" + name)
        log("unpacking " + name)
        extract(files[name], d, strip)
        return d

    # ---------------------------------------------------------------- 3. dependencies, all built with the bundled zig
    log("building zlib")
    # zlib 1.3.1's CMake always builds the shared library too (on Windows that needs a resource compiler); only the static one
    # is wanted, so build that target and install its three files by hand.
    zsrc = src("zlib")
    zbd = os.path.join(work, "build-zlib")
    run(["cmake", "-S", zsrc, "-B", zbd, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + tcf,
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"], env=env)
    run(["cmake", "--build", zbd, "-j", j, "--target", "zlibstatic"], env=env)
    os.makedirs(os.path.join(deps, "lib"), exist_ok=True)
    os.makedirs(os.path.join(deps, "include"), exist_ok=True)
    built = [f for f in os.listdir(zbd) if f in ("libz.a", "libzlibstatic.a", "libzlib.a")]
    if not built:
        die("zlib static library not found in " + zbd)
    shutil.copy2(os.path.join(zbd, built[0]), os.path.join(deps, "lib", "libz.a"))
    shutil.copy2(os.path.join(zsrc, "zlib.h"), os.path.join(deps, "include", "zlib.h"))
    shutil.copy2(os.path.join(zbd, "zconf.h"), os.path.join(deps, "include", "zconf.h"))
    log("building FreeType")
    cmake_build("freetype", src("freetype"), [
        "-DBUILD_SHARED_LIBS=OFF", "-DFT_DISABLE_ZLIB=ON", "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON",
        "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON"])
    log("building SDL2")
    cmake_build("sdl2", src("sdl2"), [
        "-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TEST=OFF", "-DSDL_TESTS=OFF", "-DSDL_INSTALL_TESTS=OFF",
        "-DSDL_ASSERTIONS=disabled", "-DSDL_LIBSAMPLERATE=OFF"])

    # Vulkan backend: the Vulkan headers (the loader is opened at run time through SDL, so there is nothing to link) and glslang,
    # which compiles the GLSL to SPIR-V inside the runtime. No optimizer (the code sets disableOptimizer; the driver optimises),
    # so SPIRV-Tools is not needed.
    log("installing the Vulkan headers")
    cmake_build("vulkan_headers", src("vulkan_headers"), [])
    log("building glslang")
    cmake_build("glslang", src("glslang"), [
        "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_EXTERNAL=OFF", "-DENABLE_OPT=OFF", "-DENABLE_HLSL=OFF", "-DENABLE_GLSLANG_BINARIES=OFF",
        "-DENABLE_CTEST=OFF", "-DGLSLANG_TESTS=OFF", "-DENABLE_PCH=OFF", "-DGLSLANG_ENABLE_INSTALL=ON"])

    log("building mbedTLS (HTTPS for cloud sync)")
    cmake_build("mbedtls", src("mbedtls"), [
        "-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF", "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF", "-DUSE_STATIC_MBEDTLS_LIBRARY=ON",
        "-DGEN_FILES=OFF"])

    ffver = None
    if not a.skip_ffmpeg:
        log("building FFmpeg (libavcodec: ATRAC3/ATRAC3+/H.264 decoders only)")
        fsrc = src("ffmpeg")
        if plat.startswith("windows"):
            # configure adds "-Wl,--pic-executable,-e,mainCRTStartup" for its own executables (none are built here); zig's lld
            # front end rejects --pic-executable, which makes every link test in configure fail.
            cfgp = os.path.join(fsrc, "configure")
            txt = open(cfgp).read()
            if "-Wl,--pic-executable," not in txt:
                die("ffmpeg configure no longer has the --pic-executable flag: drop this workaround")
            open(cfgp, "w").write(txt.replace("-Wl,--pic-executable,", "-Wl,"))
        fbd = os.path.join(work, "build-ffmpeg")
        os.makedirs(fbd)
        thr = "--enable-w32threads" if plat.startswith("windows") else "--enable-pthreads"
        run([os.path.join(fsrc, "configure"), "--prefix=" + deps,
             "--cc=%s/zig-cc" % hb, "--cxx=%s/zig-c++" % hb, "--ar=%s/zig-ar" % hb, "--ranlib=%s/zig-ranlib" % hb,
             "--enable-cross-compile", "--arch=x86_64", "--target-os=" + pinfo["ffmpeg_target_os"],
             "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-debug",
             "--disable-network", "--disable-avformat", "--disable-avdevice", "--disable-avfilter",
             "--disable-swscale", "--disable-swresample", "--disable-x86asm",
             "--enable-avcodec", "--enable-avutil", "--enable-decoder=atrac3,atrac3p,h264", thr,
             "--enable-static", "--disable-shared"] + ([] if plat.startswith("windows") else ["--enable-pic"]) + [   # mingw: no PIC (configure then passes lld an option zig rejects)
             "--extra-cflags=-fno-sanitize=undefined"], env=env, cwd=fbd)
        # configure finds sysctl() in zig's glibc stubs (glibc 2.28 still exported it) but the headers no longer have
        # <sys/sysctl.h>; nothing in a decoder-only build needs it.
        cfg = os.path.join(fbd, "config.h")
        txt = open(cfg).read()
        txt = txt.replace("#define HAVE_SYSCTL 1", "#define HAVE_SYSCTL 0")
        open(cfg, "w").write(txt)
        run(["make", "-j", j], env=env, cwd=fbd)
        run(["make", "install"], env=env, cwd=fbd)
        ffver = ver["sources"]["ffmpeg"]["version"]

    # ---------------------------------------------------------------- 3b. pspdecrypt (a tool for the installer, not a runtime dependency)
    # It needs OpenSSL's libcrypto for SHA-256 (ipl_decrypt.cpp) and DES-CBC (pspdecrypt_lib.cpp) only, so a minimal static
    # libcrypto is built in the scratch dir (never installed into the bundle) and linked into the one executable.
    log("building OpenSSL libcrypto (static, for pspdecrypt)")
    osrc = src("openssl")
    ossl_target = "mingw64" if plat.startswith("windows") else "linux-x86_64"
    run([os.path.join(osrc, "Configure"), ossl_target, "no-shared", "no-asm", "no-tests", "no-threads",
         "no-dso", "no-engine", "no-module", "no-legacy", "no-autoload-config", "no-ssl", "no-tls", "no-dtls", "no-comp", "no-ocsp",
         "no-cms", "no-ts", "no-ct", "no-srp", "no-psk",
         "--prefix=" + os.path.join(work, "openssl-inst"), "--libdir=lib",
         "CC=%s/zig-cc" % hb, "AR=%s/zig-ar" % hb, "ARFLAGS=r", "RANLIB=%s/zig-ranlib" % hb, "CFLAGS=-fno-sanitize=undefined"],
        env=env, cwd=osrc)
    run(["make", "-j", j, "build_libs"], env=env, cwd=osrc)
    log("building pspdecrypt")
    psrc = src("pspdecrypt")
    pobj = os.path.join(work, "pspdecrypt-obj")
    os.makedirs(pobj)
    objs = []
    for sub in ("", "libkirk"):
        for fn in sorted(os.listdir(os.path.join(psrc, sub))):
            base, ext = os.path.splitext(fn)
            if ext not in (".c", ".cpp") or (sub == "" and base == "kl4e_test"):
                continue
            o = os.path.join(pobj, (sub + "_" if sub else "") + base + ".o")
            cc = "zig-cc" if ext == ".c" else "zig-c++"
            run([os.path.join(hb, cc), "-O3", "-fno-sanitize=undefined", "-I" + osrc + "/include", "-I" + os.path.join(deps, "include"),
                 "-I" + psrc, "-c", os.path.join(psrc, sub, fn), "-o", o], env=env)
            objs.append(o)
    run([os.path.join(hb, "zig-c++"), "-o", os.path.join(out, "bin", "pspdecrypt" + exe)] + objs +
        [os.path.join(deps, "lib", "libz.a"), os.path.join(osrc, "libcrypto.a")] + (["-lws2_32", "-lcrypt32"] if plat.startswith("windows") else []),
        env=env)
    if os.path.isfile(os.path.join(out, "bin", "pspdecrypt.pdb")):   # lld's debug-info file next to the Windows exe
        os.remove(os.path.join(out, "bin", "pspdecrypt.pdb"))
    shutil.copy2(os.path.join(psrc, "LICENSE.TXT"), os.path.join(out, "bin", "pspdecrypt-LICENSE.txt"))

    # License texts of the libraries linked into the game the player builds (release/assemble.py copies licenses/ into the release).
    os.makedirs(os.path.join(out, "licenses"), exist_ok=True)
    for lname, lfiles in (("ffmpeg", ["COPYING.LGPLv2.1", "LICENSE.md"]), ("sdl2", ["LICENSE.txt"]), ("zlib", ["LICENSE"]),
                          ("freetype", ["docs/FTL.TXT", "docs/LICENSE.TXT"]), ("openssl", ["LICENSE.txt"]),
                          ("mbedtls", ["LICENSE", "apache-2.0.txt"])):
        for lf in lfiles:
            lp = os.path.join(work, "src-" + lname, lf)
            if os.path.isfile(lp):
                shutil.copy2(lp, os.path.join(out, "licenses", "%s-%s" % (lname, os.path.basename(lf))))

    # ---------------------------------------------------------------- 4. static libraries only
    libdir = os.path.join(deps, "lib")
    for fn in os.listdir(libdir):
        p = os.path.join(libdir, fn)
        if re.search(r"\.(so(\.|$)|dll\.a$|dylib$)", fn) and os.path.isfile(p) or os.path.islink(p):
            os.remove(p)
    for sub in ("bin", "share"):
        shutil.rmtree(os.path.join(deps, sub), ignore_errors=True)

    def lib(*names):
        for n in names:
            if os.path.isfile(os.path.join(libdir, n)):
                return n
        die("none of %s found in %s (have: %s)" % (names, libdir, sorted(os.listdir(libdir))))

    glslang_libs = [lib("libglslang.a"), lib("libSPIRV.a"), lib("libMachineIndependent.a"), lib("libGenericCodeGen.a"),
                    lib("libglslang-default-resource-limits.a")] + \
        ([lib("libOSDependent.a")] if os.path.isfile(os.path.join(libdir, "libOSDependent.a")) else [])
    sdl_lib = lib("libSDL2.a")
    ft_lib = lib("libfreetype.a")
    z_lib = lib("libz.a", "libzlibstatic.a", "libzlib.a")
    mbed_libs = [lib("libmbedtls.a"), lib("libmbedx509.a"), lib("libmbedcrypto.a")] + \
        [n for n in ("libeverest.a", "libp256m.a") if os.path.isfile(os.path.join(libdir, n))]
    mbed_sys = ["ws2_32", "bcrypt"] if plat.startswith("windows") else []
    sdl_sys = pc_private_libs(os.path.join(libdir, "pkgconfig", "sdl2.pc")) or \
        (["m", "dl", "pthread", "rt"] if plat.startswith("linux") else [])
    sdl_sys = [l for l in sdl_sys if l not in ("SDL2", "SDL2main", "mingw32")]
    av_sys = (["m", "pthread"] if plat.startswith("linux") else ["bcrypt"])

    # ---------------------------------------------------------------- 5. deps.cmake / toolchain.cmake / toolchain.json
    def cm_list(items):
        return ";".join(items)

    with open(os.path.join(out, "deps.cmake"), "w") as f:
        f.write("""# Generated by build_bundle.py. The dependency targets under the names the runtime's CMake uses for the pkg-config ones.
get_filename_component(_psp_deps "${CMAKE_CURRENT_LIST_DIR}/deps" ABSOLUTE)
set(PSPRECOMP_BUNDLED_DEPS TRUE)
function(_psp_dep name incdirs libs)
    add_library(PkgConfig::${name} INTERFACE IMPORTED GLOBAL)
    set_property(TARGET PkgConfig::${name} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${incdirs}")
    set_property(TARGET PkgConfig::${name} PROPERTY INTERFACE_LINK_LIBRARIES "${libs}")
endfunction()
""")
        f.write('_psp_dep(SDL2 "${_psp_deps}/include/SDL2" "${_psp_deps}/lib/%s;%s")\n' % (sdl_lib, cm_list(sdl_sys)))
        f.write('_psp_dep(FREETYPE "${_psp_deps}/include/freetype2" "${_psp_deps}/lib/%s")\n' % ft_lib)
        f.write('set(SDL2_FOUND TRUE)\nset(SDL2_INCLUDE_DIRS "${_psp_deps}/include/SDL2")\nset(SDL2_VERSION "%s")\n' % ver["sources"]["sdl2"]["version"])
        f.write('set(FREETYPE_FOUND TRUE)\nset(FREETYPE_VERSION "%s")\n' % ver["sources"]["freetype"]["version"])
        f.write('_psp_dep(MBEDTLS "${_psp_deps}/include" "%s;%s")\n' %
                (";".join("${_psp_deps}/lib/" + n for n in mbed_libs), cm_list(mbed_sys)))
        f.write('set(MBEDTLS_FOUND TRUE)\nset(MBEDTLS_VERSION "%s")\n' % ver["sources"]["mbedtls"]["version"])
        # Vulkan backend (runtime/CMakeLists.txt): headers + glslang. libvulkan is never linked (psp_vk_loader.cpp).
        f.write('_psp_dep(GLSLANG "${_psp_deps}/include;${_psp_deps}/include/glslang" "%s;%s")\n' %
                (";".join("${_psp_deps}/lib/" + n for n in glslang_libs), "pthread" if plat.startswith("linux") else ""))
        f.write('set(GLSLANG_FOUND TRUE)\nset(VULKAN_HEADERS_FOUND TRUE)\n')
        if ffver:
            f.write('_psp_dep(AVCODEC "${_psp_deps}/include" "${_psp_deps}/lib/%s;${_psp_deps}/lib/%s;%s")\n' %
                    (lib("libavcodec.a"), lib("libavutil.a"), cm_list(av_sys)))
            f.write('set(AVCODEC_FOUND TRUE)\nset(AVCODEC_VERSION "%s")\n' % ffver)
        else:
            f.write("set(AVCODEC_FOUND FALSE)\n")
        f.write('add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)\n'
                'set_target_properties(ZLIB::ZLIB PROPERTIES IMPORTED_LOCATION "${_psp_deps}/lib/%s" INTERFACE_INCLUDE_DIRECTORIES "${_psp_deps}/include")\n'
                'set(ZLIB_FOUND TRUE)\n' % z_lib)

    with open(os.path.join(out, "toolchain.cmake"), "w") as f:
        f.write("""# Generated by build_bundle.py: the bundle's CMake toolchain file.  cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<bundle>/toolchain.cmake ...
# Everything built with it (the runtime core in CI, the player's generated code at install time) uses the same zig clang +
# libc++, which is what makes the prebuilt core linkable with code compiled on another machine.
get_filename_component(_psp_tc "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
set(CMAKE_C_COMPILER   "${_psp_tc}/bin/zig-cc%(exe)s")
set(CMAKE_CXX_COMPILER "${_psp_tc}/bin/zig-c++%(exe)s")
set(CMAKE_AR     "${_psp_tc}/bin/zig-ar%(exe)s" CACHE FILEPATH "")
set(CMAKE_RANLIB "${_psp_tc}/bin/zig-ranlib%(exe)s" CACHE FILEPATH "")
set(CMAKE_RC_COMPILER "${_psp_tc}/bin/zig-rc%(exe)s")
set(CMAKE_MAKE_PROGRAM "${_psp_tc}/bin/ninja%(exe)s" CACHE FILEPATH "")
set(CMAKE_C_FLAGS_INIT "-fno-sanitize=undefined")
set(CMAKE_CXX_FLAGS_INIT "-fno-sanitize=undefined")
set(PSPRECOMP_DEPS_CMAKE "${_psp_tc}/deps.cmake")
set(PSPRECOMP_TOOLCHAIN_JSON "${_psp_tc}/toolchain.json")
""" % {"exe": exe})

    meta = {
        "bundle_version": ver["bundle_version"],
        "platform": plat,
        "zig_target": pinfo["zig_target"],
        "zig": ver["zig"]["version"],
        "cmake": ver["cmake"]["version"],
        "ninja": ver["ninja"]["version"],
        "libraries": {n: e["version"] for n, e in ver["sources"].items() if n != "ffmpeg" or ffver},
    }
    json.dump(meta, open(os.path.join(out, "toolchain.json"), "w"), indent=2)

    # ---------------------------------------------------------------- 6. smoke test (native bundles only)
    if not cross:
        log("smoke test: C++17 + SDL2 + FreeType + zlib%s" % (" + libavcodec" if ffver else ""))
        t = os.path.join(work, "smoke")
        os.makedirs(t)
        code = ["#include <string>", "#include <map>", "#include <thread>", "#include <cstdio>", "#include <SDL.h>",
                "#include <ft2build.h>", "#include FT_FREETYPE_H", "#include <zlib.h>"]
        if ffver:
            code += ['extern "C" {', "#include <libavcodec/avcodec.h>", "}"]
        code += ["int main() {",
                 '  std::map<std::string, int> m{{"a", 1}}; FT_Library l; FT_Init_FreeType(&l);',
                 "  SDL_version v; SDL_GetVersion(&v);",
                 '  std::printf("libc++ ok (%zu) SDL %d.%d.%d zlib %s\\n", m.size(), v.major, v.minor, v.patch, zlibVersion());']
        if ffver:
            code += ['  std::printf("atrac3=%d atrac3p=%d h264=%d\\n", avcodec_find_decoder(AV_CODEC_ID_ATRAC3) != nullptr,',
                     "      avcodec_find_decoder(AV_CODEC_ID_ATRAC3P) != nullptr, avcodec_find_decoder(AV_CODEC_ID_H264) != nullptr);"]
        code += ["  return 0;", "}"]
        open(os.path.join(t, "main.cpp"), "w").write("\n".join(code) + "\n")
        cxx = os.path.join(out, "bin", "zig-c++")
        incs = ["-I" + os.path.join(deps, "include"), "-I" + os.path.join(deps, "include", "SDL2"),
                "-I" + os.path.join(deps, "include", "freetype2")]
        link = [os.path.join(libdir, n) for n in ([sdl_lib, ft_lib, z_lib] + (["libavcodec.a", "libavutil.a"] if ffver else []))]
        link += ["-l" + l for l in sdl_sys + av_sys]
        run([cxx, "-std=c++17", "-O2", os.path.join(t, "main.cpp"), "-o", os.path.join(t, "smoke")] + incs + link, env=env)
        run([os.path.join(t, "smoke")], env=env)

    if cross:
        # What CI needs to build FOR this platform on this host: the host-runnable tools (zig/cmake/ninja/launchers that target the
        # bundle's platform) and a toolchain file using them with the bundle's deps. Kept next to the bundle as <out>.host/.
        hostdir = out + ".host"
        if os.path.isdir(hostdir):
            shutil.rmtree(hostdir)
        shutil.move(os.path.join(work, "host"), hostdir)
        hb2 = os.path.join(hostdir, "bin")
        with open(os.path.join(hostdir, "toolchain-cross.cmake"), "w") as f:
            f.write("# Generated by build_bundle.py: build for %s on this (Linux) host with the bundle's compiler.\n" % plat)
            f.write("set(CMAKE_SYSTEM_NAME %s)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n" % pinfo["cmake_system"])
            f.write('set(CMAKE_C_COMPILER "%s/zig-cc")\nset(CMAKE_CXX_COMPILER "%s/zig-c++")\n' % (hb2, hb2))
            f.write('set(CMAKE_AR "%s/zig-ar" CACHE FILEPATH "")\nset(CMAKE_RANLIB "%s/zig-ranlib" CACHE FILEPATH "")\n' % (hb2, hb2))
            f.write('set(CMAKE_RC_COMPILER "%s/zig-rc")\nset(CMAKE_MAKE_PROGRAM "%s/ninja" CACHE FILEPATH "")\n' % (hb2, hb2))
            f.write('set(CMAKE_C_FLAGS_INIT "-fno-sanitize=undefined")\nset(CMAKE_CXX_FLAGS_INIT "-fno-sanitize=undefined")\n')
            f.write('set(PSPRECOMP_DEPS_CMAKE "%s/deps.cmake")\nset(PSPRECOMP_TOOLCHAIN_JSON "%s/toolchain.json")\n' % (out, out))
        log("cross-build tools kept in " + hostdir)
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    log("bundle ready: " + out)


if __name__ == "__main__":
    main()
