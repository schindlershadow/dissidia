#!/usr/bin/env python3
"""Build the libraries the game needs for Android arm64 with the NDK: SDL2 (shared), FreeType, FFmpeg's libavcodec (ATRAC3/ATRAC3+/H.264
decoders only, as in toolchain/build_bundle.py), and write deps.cmake with the targets the runtime's CMake expects.

  build_deps.py --sdk SDK --cache DIR(sources, as tcb/cache) --cmake CMAKE --ninja NINJA --out DIR [--api 26]

Result: DIR/deps/{include,lib}, DIR/deps.cmake, DIR/toolchain.cmake (use as -DCMAKE_TOOLCHAIN_FILE for the core and the game).
Versions are the ones in toolchain/versions.json (same pinned sources).
"""
import argparse, glob, json, os, shutil, subprocess, tarfile

def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, **kw)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True); ap.add_argument("--cache", required=True)
    ap.add_argument("--cmake", default="cmake"); ap.add_argument("--ninja", default="ninja")
    ap.add_argument("--out", required=True); ap.add_argument("--api", default="26")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    ver = json.load(open(os.path.join(here, "..", "toolchain", "versions.json")))["sources"]
    sdk = os.path.abspath(a.sdk); out = os.path.abspath(a.out); deps = out + "/deps"
    os.makedirs(deps, exist_ok=True)
    pinned = json.load(open(os.path.join(here, "versions.json")))["ndk"]["version"]   # NOT the newest NDK in the folder: a CI image carries a newer one, whose libc++ the phone's sysroot (r27c) does not match
    ndk = os.path.join(sdk, "ndk", pinned)
    if not os.path.isdir(ndk):
        raise SystemExit("build_deps: NDK %s not found under %s/ndk" % (pinned, sdk))
    tc = ndk + "/build/cmake/android.toolchain.cmake"
    bin_ = ndk + "/toolchains/llvm/prebuilt/linux-x86_64/bin"
    cc = "%s/aarch64-linux-android%s-clang" % (bin_, a.api)
    env = dict(os.environ, PATH=os.path.dirname(os.path.abspath(a.ninja)) + os.pathsep + bin_ + os.pathsep + os.environ["PATH"])
    j = str(os.cpu_count())

    def unpack(name):
        url = ver[name]["url"]
        f = os.path.join(a.cache, url.rsplit("/", 1)[1])
        d = os.path.join(out, "src-" + name)
        if not os.path.isdir(d):
            os.makedirs(d)
            with tarfile.open(f) as t:
                members = t.getmembers()
                for m in members:
                    m.name = m.name.split("/", 1)[1] if "/" in m.name else m.name
                t.extractall(d, members=[m for m in members if m.name])
        return d

    def cmake_dep(name, extra):
        src = unpack(name); bd = out + "/build-" + name
        run([a.cmake, "-S", src, "-B", bd, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + tc, "-DANDROID_ABI=arm64-v8a",
             "-DANDROID_PLATFORM=android-" + a.api, "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_INSTALL_PREFIX=" + deps,
             "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"] + extra, env=env)
        run([a.cmake, "--build", bd, "-j", j], env=env); run([a.cmake, "--install", bd], env=env)

    if not os.path.isfile(deps + "/lib/libfreetype.a"):
        cmake_dep("freetype", ["-DFT_DISABLE_ZLIB=ON", "-DFT_DISABLE_BZIP2=ON", "-DFT_DISABLE_PNG=ON", "-DFT_DISABLE_HARFBUZZ=ON", "-DFT_DISABLE_BROTLI=ON"])
    if not os.path.isfile(deps + "/lib/libSDL2.so"):
        cmake_dep("sdl2", ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF", "-DSDL_TESTS=OFF", "-DSDL_INSTALL_TESTS=OFF"])
    if not os.path.isfile(deps + "/lib/libmbedtls.a"):   # HTTPS for cloud sync (psp_http); the CA roots are embedded in the runtime
        cmake_dep("mbedtls", ["-DENABLE_TESTING=OFF", "-DENABLE_PROGRAMS=OFF", "-DUSE_SHARED_MBEDTLS_LIBRARY=OFF",
                              "-DUSE_STATIC_MBEDTLS_LIBRARY=ON", "-DGEN_FILES=OFF"])
    if not os.path.isfile(deps + "/lib/libavcodec.a"):
        fsrc = unpack("ffmpeg"); fbd = out + "/build-ffmpeg"
        shutil.rmtree(fbd, ignore_errors=True); os.makedirs(fbd)
        run([fsrc + "/configure", "--prefix=" + deps, "--enable-cross-compile", "--arch=aarch64", "--target-os=android", "--cc=" + cc,
             "--cxx=" + cc + "++", "--ar=" + bin_ + "/llvm-ar", "--ranlib=" + bin_ + "/llvm-ranlib", "--strip=" + bin_ + "/llvm-strip", "--nm=" + bin_ + "/llvm-nm",
             "--disable-everything", "--disable-autodetect", "--disable-programs", "--disable-doc", "--disable-debug", "--disable-network",
             "--disable-avformat", "--disable-avdevice", "--disable-avfilter", "--disable-swscale", "--disable-swresample",
             "--enable-avcodec", "--enable-avutil", "--enable-decoder=atrac3,atrac3p,h264", "--enable-pthreads", "--enable-static", "--disable-shared", "--enable-pic", "--disable-asm"]   # the NEON asm is not PIC (libmain.so is a shared library)
        ,
            env=env, cwd=fbd)
        run(["make", "-j", j], env=env, cwd=fbd); run(["make", "install"], env=env, cwd=fbd)
    if not os.path.isfile(deps + "/lib/libglslang.a"):   # GLSL -> SPIR-V at run time: shaders missing from the SPIR-V cache are compiled on the device
        cmake_dep("glslang", ["-DBUILD_SHARED_LIBS=OFF", "-DBUILD_EXTERNAL=OFF", "-DENABLE_OPT=OFF", "-DENABLE_HLSL=OFF", "-DENABLE_GLSLANG_BINARIES=OFF",
                              "-DENABLE_CTEST=OFF", "-DGLSLANG_TESTS=OFF", "-DENABLE_PCH=OFF", "-DGLSLANG_ENABLE_INSTALL=ON"])
    if not os.path.isfile(deps + "/lib/libadrenotools.a"):   # custom GPU drivers (Adreno): static library + four hook libraries the APK carries
        asrc = unpack("libadrenotools"); lsrc = unpack("liblinkernsbypass")
        shutil.rmtree(asrc + "/lib/linkernsbypass", ignore_errors=True)
        shutil.copytree(lsrc, asrc + "/lib/linkernsbypass")   # a git submodule upstream: not in the source tarball
        bd = out + "/build-libadrenotools"
        run([a.cmake, "-S", asrc, "-B", bd, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + tc, "-DANDROID_ABI=arm64-v8a",
             "-DANDROID_PLATFORM=android-" + a.api, "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"], env=env)
        run([a.cmake, "--build", bd, "-j", j], env=env)
        os.makedirs(deps + "/lib", exist_ok=True)
        for pat in ("libadrenotools.a", "lib/linkernsbypass/liblinkernsbypass.a"):
            shutil.copy2(bd + "/" + pat, deps + "/lib/" + os.path.basename(pat))
        for so in ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so"):
            hit = glob.glob(bd + "/**/" + so, recursive=True)
            if not hit:
                raise SystemExit("libadrenotools: %s was not built" % so)
            shutil.copy2(hit[0], deps + "/lib/" + so)
        shutil.copytree(asrc + "/include/adrenotools", deps + "/include/adrenotools", dirs_exist_ok=True)
    mbed = ["libmbedtls.a", "libmbedx509.a", "libmbedcrypto.a"] + [n for n in ("libeverest.a", "libp256m.a") if os.path.isfile(deps + "/lib/" + n)]
    with open(out + "/deps.cmake", "w") as f:
        f.write('''# Generated by android/build_deps.py: Android arm64 dependency targets under the names the runtime's CMake uses.
get_filename_component(_psp_deps "${CMAKE_CURRENT_LIST_DIR}/deps" ABSOLUTE)
set(PSPRECOMP_BUNDLED_DEPS TRUE)
function(_psp_dep name incdirs libs)
    add_library(PkgConfig::${name} INTERFACE IMPORTED GLOBAL)
    set_property(TARGET PkgConfig::${name} PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${incdirs}")
    set_property(TARGET PkgConfig::${name} PROPERTY INTERFACE_LINK_LIBRARIES "${libs}")
endfunction()
_psp_dep(SDL2 "${_psp_deps}/include/SDL2" "${_psp_deps}/lib/libSDL2.so")
_psp_dep(FREETYPE "${_psp_deps}/include/freetype2" "${_psp_deps}/lib/libfreetype.a")
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "${_psp_deps}/include/SDL2")
set(FREETYPE_FOUND TRUE)
_psp_dep(AVCODEC "${_psp_deps}/include" "${_psp_deps}/lib/libavcodec.a;${_psp_deps}/lib/libavutil.a;m")
set(AVCODEC_FOUND TRUE)
_psp_dep(MBEDTLS "${_psp_deps}/include" "%s")
set(MBEDTLS_FOUND TRUE)
set(MBEDTLS_VERSION "%s")
if(EXISTS "${_psp_deps}/lib/libglslang.a")   # the Vulkan backend compiles GLSL on the device (no SPIR-V cache needed)
    set(_psp_glslang_libs "")
    foreach(_n glslang SPIRV MachineIndependent GenericCodeGen glslang-default-resource-limits OSDependent)
        if(EXISTS "${_psp_deps}/lib/lib${_n}.a")
            list(APPEND _psp_glslang_libs "${_psp_deps}/lib/lib${_n}.a")
        endif()
    endforeach()
    _psp_dep(GLSLANG "${_psp_deps}/include;${_psp_deps}/include/glslang" "${_psp_glslang_libs}")
endif()
if(EXISTS "${_psp_deps}/lib/libadrenotools.a")   # custom GPU drivers (Adreno)
    _psp_dep(ADRENOTOOLS "${_psp_deps}/include" "${_psp_deps}/lib/libadrenotools.a;${_psp_deps}/lib/liblinkernsbypass.a;android;dl;log")
endif()
find_library(_psp_z z)
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_LINK_LIBRARIES "${_psp_z}")
set(ZLIB_FOUND TRUE)
''' % (";".join("${_psp_deps}/lib/" + n for n in mbed), ver["mbedtls"]["version"]))
    # The libraries the game links after the core, in link order (what deps.cmake's targets add: AVCODEC, GLSLANG, MBEDTLS, ADRENOTOOLS), for the
    # on-device build (games/dissidia/tools/android_build.py device_build), which has no CMake: one entry per line, "-lname" = a system library.
    link = ["libavcodec.a", "libavutil.a", "-lm"]
    link += ["lib%s.a" % n for n in ("glslang", "SPIRV", "MachineIndependent", "GenericCodeGen", "glslang-default-resource-limits", "OSDependent")
             if os.path.isfile(deps + "/lib/lib%s.a" % n)]
    link += mbed
    if os.path.isfile(deps + "/lib/libadrenotools.a"):
        link += ["libadrenotools.a", "liblinkernsbypass.a", "-landroid", "-ldl", "-llog"]
    with open(out + "/link.txt", "w") as f:
        f.write("\n".join(link) + "\n")
    with open(out + "/toolchain.cmake", "w") as f:
        f.write('set(ANDROID_ABI arm64-v8a)\nset(ANDROID_PLATFORM android-%s)\ninclude("%s")\nset(PSPRECOMP_DEPS_CMAKE "%s/deps.cmake")\n' % (a.api, tc, out))
    print("deps ready in", out)

if __name__ == "__main__":
    main()
