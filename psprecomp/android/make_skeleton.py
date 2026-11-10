#!/usr/bin/env python3
"""Build the unsigned APK skeleton (manifest + compiled resources + classes.dex of SDL's Java activity), zip-aligned, for apk_pack.py.
Release-time tool (needs a JDK, the Android SDK build-tools + platform, SDL2 source): the player's machine only runs apk_pack.py.

  make_skeleton.py --sdk SDK --sdl-src SDL2_SRC [--package org.psprecomp.game] [--label "Dissidia 012"] [--api 29] -o skeleton.apk
"""
import argparse, glob, os, shutil, subprocess, sys, tempfile


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.check_call(cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--sdk", required=True); ap.add_argument("--sdl-src", required=True)
    ap.add_argument("--package", default="org.psprecomp.game"); ap.add_argument("--label", default="Dissidia 012")
    ap.add_argument("--api", default="29"); ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    sdk = os.path.abspath(a.sdk)
    bt = sorted(glob.glob(sdk + "/build-tools/*"))[-1]
    jar = sorted(glob.glob(sdk + "/platforms/android-*/android.jar"))[-1]
    work = tempfile.mkdtemp(prefix="skeleton-")
    classes = work + "/classes"; os.makedirs(classes)
    srcs = glob.glob(os.path.abspath(a.sdl_src) + "/android-project/app/src/main/java/org/libsdl/app/*.java")
    run(["javac", "-source", "8", "-target", "8", "-Xlint:-options", "-cp", jar, "-d", classes] + srcs)
    cls = glob.glob(classes + "/**/*.class", recursive=True)
    run([bt + "/d8", "--lib", jar, "--min-api", a.api, "--output", work] + cls)
    man = work + "/AndroidManifest.xml"
    txt = open(here + "/hello/AndroidManifest.xml").read().replace("org.psprecomp.hello", a.package).replace("psprecomp hello", a.label)
    open(man, "w").write(txt)
    base = work + "/base.apk"
    run([bt + "/aapt2", "link", "-o", base, "-I", jar, "--manifest", man, "--min-sdk-version", a.api, "--target-sdk-version", "34"])
    import zipfile
    unaligned = work + "/unaligned.apk"
    shutil.copy(base, unaligned)
    with zipfile.ZipFile(unaligned, "a", zipfile.ZIP_DEFLATED) as z:
        z.write(work + "/classes.dex", "classes.dex")
    if os.path.exists(a.output):
        os.remove(a.output)
    run([bt + "/zipalign", "-p", "-f", "4", unaligned, os.path.abspath(a.output)])
    shutil.rmtree(work, ignore_errors=True)
    print("skeleton", a.output)


if __name__ == "__main__":
    main()
