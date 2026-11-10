#!/usr/bin/env python3
"""Build the Android hello APK (milestone 1 of task 20): SDL2 + GLES3 + input, nothing of the game.

  build_hello.py --sdk ~/dissidia/android/sdk --sdl-src <SDL2 source dir> --cmake <cmake> --ninja <ninja> --out DIR

Needs the Android SDK (build-tools;34.0.0, platforms;android-34) and NDK 27 under --sdk, a JDK (javac, keytool), cmake and ninja.
Result: DIR/hello.apk (debug-signed; install: adb install -r DIR/hello.apk).
"""
import argparse, glob, os, shutil, subprocess, sys, zipfile

def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd, **kw)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sdk", required=True); ap.add_argument("--sdl-src", required=True)
    ap.add_argument("--cmake", default="cmake"); ap.add_argument("--ninja", default="ninja")
    ap.add_argument("--out", required=True); ap.add_argument("--api", default="26")
    ap.add_argument("--main-so", help="a prebuilt libmain.so (the game): skips building the hello app")
    ap.add_argument("--sdl-so", help="libSDL2.so to pack with --main-so")
    ap.add_argument("--package", default="org.psprecomp.hello"); ap.add_argument("--label", default="psprecomp hello")
    ap.add_argument("--apk-name", default="hello.apk")
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    sdk = os.path.abspath(a.sdk); out = os.path.abspath(a.out); os.makedirs(out, exist_ok=True)
    ndk = sorted(glob.glob(sdk + "/ndk/*"))[-1]
    bt = sorted(glob.glob(sdk + "/build-tools/*"))[-1]
    jar = sorted(glob.glob(sdk + "/platforms/android-*/android.jar"))[-1]
    tc = ndk + "/build/cmake/android.toolchain.cmake"
    env = dict(os.environ, PATH=os.path.dirname(os.path.abspath(a.ninja)) + os.pathsep + os.environ["PATH"])
    # 1. SDL2 (shared) + the app library "main" for arm64-v8a
    bd = out + "/build"
    os.makedirs(bd, exist_ok=True)
    cm = [a.cmake, "-S", here + "/hello", "-B", bd, "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=" + tc, "-DANDROID_ABI=arm64-v8a",
          "-DANDROID_PLATFORM=android-" + a.api, "-DCMAKE_BUILD_TYPE=Release", "-DSDL2_SRC=" + os.path.abspath(a.sdl_src), "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"]
    if not a.main_so:
        run(cm, env=env); run([a.cmake, "--build", bd, "-j", str(os.cpu_count())], env=env)
    # 2. Java side: SDL's activity classes -> classes.dex
    jd = out + "/java"; shutil.rmtree(jd, ignore_errors=True); os.makedirs(jd + "/classes")
    srcs = glob.glob(os.path.abspath(a.sdl_src) + "/android-project/app/src/main/java/org/libsdl/app/*.java")
    run(["javac", "-source", "8", "-target", "8", "-Xlint:-options", "-cp", jar, "-d", jd + "/classes"] + srcs)
    classes = glob.glob(jd + "/classes/**/*.class", recursive=True)
    run([bt + "/d8", "--lib", jar, "--min-api", a.api, "--output", jd] + classes)
    # 3. manifest + resources -> base apk, then add dex and libs
    apk0 = out + "/base.apk"
    man = out + "/AndroidManifest.xml"
    txt = open(here + "/hello/AndroidManifest.xml").read().replace("org.psprecomp.hello", a.package).replace("psprecomp hello", a.label)
    open(man, "w").write(txt)
    run([bt + "/aapt2", "link", "-o", apk0, "-I", jar, "--manifest", man, "--min-sdk-version", a.api, "--target-sdk-version", "34"])
    apk1 = out + "/unaligned.apk"
    shutil.copy(apk0, apk1)
    with zipfile.ZipFile(apk1, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(jd + "/classes.dex", "classes.dex")
        for so in ("libSDL2.so", "libmain.so"):
            p = (a.sdl_so if so == "libSDL2.so" else a.main_so) if a.main_so else glob.glob(bd + "/**/" + so, recursive=True)[0]
            z.write(p, "lib/arm64-v8a/" + so)
        shutil.copy(ndk + "/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so", out + "/libc++_shared.so")
        z.write(out + "/libc++_shared.so", "lib/arm64-v8a/libc++_shared.so")
    apk2 = out + "/aligned.apk"
    if os.path.exists(apk2): os.remove(apk2)
    run([bt + "/zipalign", "-p", "-f", "4", apk1, apk2])
    ks = out + "/debug.keystore"
    if not os.path.exists(ks):
        run(["keytool", "-genkeypair", "-keystore", ks, "-storepass", "android", "-keypass", "android", "-alias", "debug", "-keyalg", "RSA",
             "-keysize", "2048", "-validity", "10000", "-dname", "CN=psprecomp debug"])
    final = out + "/" + a.apk_name
    run([bt + "/apksigner", "sign", "--ks", ks, "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", final, apk2])
    print("built", final, os.path.getsize(final) // 1024, "KB")

if __name__ == "__main__":
    main()
