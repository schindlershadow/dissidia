#!/usr/bin/env python3
"""Build the Android part of a release (the same for the Linux and Windows archives): the prebuilt Android core + libraries and the APK skeleton.

  build_android_sdk.py --sdk ANDROID_SDK --cache SOURCES --cmake CMAKE --ninja NINJA --stub HEADER_STUB_OUT --work DIR --out DIR

ANDROID_SDK has `ndk/<version of android/versions.json>`, `build-tools/*` and `platforms/android-*` (sdkmanager installs them; a JDK is needed for
javac/d8). Nothing here is game-derived. Result (see games/dissidia/tools/android_build.py for the layout the player's machine expects):
  OUT/versions.json android.json sdk/ deps/ apk/{skeleton.apk,apk_pack.py,debug_key.json} libc++_shared.so
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))


def run(cmd, env=None, cwd=None):
    print("$ " + " ".join(cmd), flush=True)
    if subprocess.run(cmd, env=env, cwd=cwd).returncode != 0:
        sys.exit("build_android_sdk: %s failed" % os.path.basename(cmd[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sdk", required=True); ap.add_argument("--cache", required=True)
    ap.add_argument("--cmake", default="cmake"); ap.add_argument("--ninja", default="ninja")
    ap.add_argument("--stub", required=True, help="the header stub output dir (psprecomp recompile --headers-only)")
    ap.add_argument("--work", required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    ver = json.load(open(os.path.join(REPO, "android", "versions.json")))
    sdk = os.path.abspath(a.sdk); work = os.path.abspath(a.work); out = os.path.abspath(a.out)
    ndk = os.path.join(sdk, "ndk", ver["ndk"]["version"])
    if not os.path.isdir(ndk):
        sys.exit("build_android_sdk: NDK %s not found under %s (sdkmanager \"ndk;%s\")" % (ver["ndk"]["version"], sdk, ver["ndk"]["version"]))
    os.makedirs(work, exist_ok=True)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([os.path.dirname(os.path.abspath(a.ninja)), os.path.dirname(os.path.abspath(a.cmake)), env["PATH"]])

    # 1. SDL2 / FreeType / FFmpeg for arm64 with the NDK
    deps = os.path.join(work, "deps")
    run([sys.executable, "-I", os.path.join(REPO, "android", "build_deps.py"), "--sdk", sdk, "--cache", a.cache, "--cmake", a.cmake,
         "--ninja", a.ninja, "--out", deps, "--api", str(ver["api"])], env=env)
    # 2. the Android core + the SDK link project
    core = os.path.join(work, "core")
    shutil.rmtree(core, ignore_errors=True)
    run([a.cmake, "-S", os.path.join(REPO, "runtime"), "-B", core, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(deps, "toolchain.cmake"), "-DPSPRECOMP_OUTPUT_DIR=" + os.path.abspath(a.stub)], env=env)
    run([a.cmake, "--build", core, "--target", "psprecomp_core"], env=env)
    run([a.cmake, "--install", core, "--component", "sdk", "--prefix", os.path.join(out, "sdk")], env=env)
    # 3. dependencies (relocatable: deps.cmake finds them next to itself)
    shutil.copytree(os.path.join(deps, "deps"), os.path.join(out, "deps", "deps"), symlinks=True)
    shutil.copy2(os.path.join(deps, "deps.cmake"), os.path.join(out, "deps", "deps.cmake"))
    # 4. APK skeleton + packer + debug key
    os.makedirs(os.path.join(out, "apk"))
    sdl_src = os.path.join(deps, "src-sdl2")
    run([sys.executable, "-I", os.path.join(REPO, "android", "make_skeleton.py"), "--sdk", sdk, "--sdl-src", sdl_src,
         "--package", ver["package"], "--api", str(ver["api"]), "-o", os.path.join(out, "apk", "skeleton.apk")], env=env)
    for f in ("apk_pack.py", "debug_key.json"):
        shutil.copy2(os.path.join(REPO, "android", f), os.path.join(out, "apk", f))
    # the shader list (android/shaders/: what phones recorded) as the pack the game compiles at start; the player's APK carries it as assets/shaders.pack
    run([sys.executable, "-I", os.path.join(REPO, "android", "pack_shaders.py"), "-o", os.path.join(out, "shaders.pack")], env=env)
    # 5. libc++ for the app, identity of what was built
    stl = glob.glob(os.path.join(ndk, "toolchains", "llvm", "prebuilt", "*", "sysroot", "usr", "lib", "aarch64-linux-android", "libc++_shared.so"))[0]
    shutil.copy2(stl, os.path.join(out, "libc++_shared.so"))
    shutil.copy2(os.path.join(REPO, "android", "versions.json"), os.path.join(out, "versions.json"))
    json.dump({"ndk": ver["ndk"]["version"], "api": ver["api"], "abi": ver["abi"]}, open(os.path.join(out, "android.json"), "w"), indent=1)
    print("android part ready:", out)


if __name__ == "__main__":
    main()
