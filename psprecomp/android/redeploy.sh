#!/bin/bash
# Rebuild the game library + APK and reinstall on the device: redeploy.sh <adb serial>   (runs on the build machine, see docs/installer.md)
set -eo pipefail
D=$1
cd ~/dissidia/android
./build_game_and.sh 2>&1 | tail -3
export PATH=$PWD/jdk/bin:$PATH
NDK=$(ls -d $PWD/sdk/ndk/*)
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o game_and/libmain.stripped.so game_and/libmain.so
[ -f skeleton.apk ] || python3 ~/code/dissidia/psprecomp/android/make_skeleton.py --sdk sdk --sdl-src sdl2src -o skeleton.apk
python3 ~/code/dissidia/psprecomp/android/apk_pack.py --skeleton skeleton.apk --lib lib/arm64-v8a/libmain.so=game_and/libmain.stripped.so \
  --lib lib/arm64-v8a/libSDL2.so=and_deps29/deps/lib/libSDL2.so --lib lib/arm64-v8a/libc++_shared.so=$HOME/dissidia/and_part/libc++_shared.so -o gameapk/dissidia.apk
adb -s $D install -r gameapk/dissidia.apk | tail -1
