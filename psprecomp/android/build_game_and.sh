#!/bin/bash
# Build libmain.so (core + the generated game) for Android arm64 on the build machine. See docs/installer.md "Android progress".
# Usage: build_game_and.sh [work folder with output/ and overlays/ (default ~/Games/Dissidia012)]   (env ANDROID_DIR = ~/dissidia/android)
set -eo pipefail
export PATH=$HOME/dissidia/tcb/v2/lin/bin:$PATH
A=${ANDROID_DIR:-$HOME/dissidia/android}; W=${1:-$HOME/Games/Dissidia012}
cd $A
TC=$A/and_deps29/toolchain.cmake   # API 29: native ELF TLS (below it the compiler emulates thread_local, ~15% of the run time)
cmake -S ~/code/dissidia/psprecomp/runtime -B core_and -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_OUTPUT_DIR=$W/output >/dev/null
cmake --build core_and --target psprecomp_core 2>&1 | tail -2
rm -rf sdk_and && cmake --install core_and --component sdk --prefix sdk_and >/dev/null
OV=""; for o in supacc battle menu extra; do [ -d $W/overlays/$o ] && OV="$OV;$W/overlays/$o"; done
cmake -S sdk_and -B game_and -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_OUTPUT_DIR=$W/output -DPSPRECOMP_OVERLAY_DIRS="${OV#;}" >/dev/null
cmake --build game_and --target psprecomp_runtime 2>&1 | tail -4
ls -la game_and/libmain.so
echo BUILD_DONE
