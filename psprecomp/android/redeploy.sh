#!/bin/bash
# Rebuild the game library + APK and reinstall on the device: redeploy.sh <adb serial>   (runs on the build machine, see docs/installer.md)
set -eo pipefail
D=$1
cd ~/dissidia/android
./build_game_and.sh 2>&1 | tail -3
export PATH=$PWD/jdk/bin:$PATH
NDK=$(ls -d $PWD/sdk/ndk/*)
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o game_and/libmain.stripped.so game_and/libmain.so
python3 ~/code/dissidia/psprecomp/android/build_hello.py --sdk sdk --sdl-src sdl2src --out gameapk --main-so game_and/libmain.stripped.so --sdl-so and_deps/deps/lib/libSDL2.so --package org.psprecomp.game --label "Dissidia 012" --apk-name dissidia.apk 2>&1 | tail -1
adb -s $D install -r gameapk/dissidia.apk | tail -1
