#!/bin/bash
# Build libmain.so (core + the generated game) for Android arm64 on the build machine. See docs/installer.md "Android progress".
# Usage: build_game_and.sh [work folder with output/ and overlays/ (default ~/Games/Dissidia012)]   (env ANDROID_DIR = ~/dissidia/android)
set -eo pipefail
export PATH=$HOME/dissidia/tcb/v2/lin/bin:$PATH
A=${ANDROID_DIR:-$HOME/dissidia/android}; W=${1:-$HOME/Games/Dissidia012}
EXTRA=${CMAKE_EXTRA:-}   # extra cmake options for both configures (e.g. -DPSPRECOMP_ALLOW_STALE=ON when building from a tree without crates/)
RT=${RUNTIME_SRC:-$HOME/code/dissidia/psprecomp/runtime}   # the runtime sources to build (env RUNTIME_SRC overrides the default checkout)
cd $A
TC=$A/and_deps29/toolchain.cmake
# NOT -ftls-model=initial-exec: bionic refuses it in a dlopen()ed library ("dlopen failed: TLS symbol (null)"). The dynamic TLS
# descriptor resolver costs ~8% of a battle: cut by touching thread_local variables less in hot paths instead.
TLS=""   # API 29: native ELF TLS (below it the compiler emulates thread_local, ~15% of the run time)
# a core_and cache configured from another source tree (another checkout, env RUNTIME_SRC) cannot be reused: start it fresh
grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$RT\$" core_and/CMakeCache.txt 2>/dev/null || rm -rf core_and
cmake -S "$RT" -B core_and -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release $TLS -DPSPRECOMP_OUTPUT_DIR=$W/output $EXTRA >/dev/null
cmake --build core_and --target psprecomp_core 2>&1 | tail -2
rm -rf sdk_and && cmake --install core_and --component sdk --prefix sdk_and >/dev/null
OV=""; for o in supacc battle menu extra; do [ -d $W/overlays/$o ] && OV="$OV;$W/overlays/$o"; done
cmake -S sdk_and -B game_and -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release $TLS -DPSPRECOMP_OUTPUT_DIR=$W/output -DPSPRECOMP_OVERLAY_DIRS="${OV#;}" $EXTRA >/dev/null
cmake --build game_and --target psprecomp_runtime 2>&1 | tail -4
ls -la game_and/libmain.so
echo BUILD_DONE
