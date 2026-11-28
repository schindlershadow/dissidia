#!/bin/bash
# collect_shaders.sh: pull the shader sources every test device recorded (<cache>/src/<hash>.<stage>.glsl; recording is on by default, PSPRECOMP_VK_RECORD=0 off)
# and merge them into android/shaders/ (the shipped shader pack list). Read-only on the devices (files are only copied off). Repeatable.
#   host      ~/.cache/dissidia-vk and every ~/dissidia/*/vk_cache or PSPRECOMP_VK_CACHE_DIR copy under /tmp/shc_*
#   desktop   ssh 192.168.2.131, Deck: ssh deck (~/.cache/dissidia-vk, ~/dissidia/*/vk_cache)
#   laptop    ssh schindler@192.168.2.55 (Windows: %LOCALAPPDATA%\dissidia-vk, else HOME\.cache\dissidia-vk)
#   RP6       adb serial containing c2aba36c (the app's files/game/vk_cache/src on shared storage); never the SHIELD (192.168.2.37)
set -u
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
args=()
add() { n=$(ls "$T/$1" 2>/dev/null | grep -c '\.glsl$'); echo "collect_shaders: $1: $n recorded"; [ "$n" -gt 0 ] && args+=(--from-dir "$T/$1"); }
mkdir -p "$T/host"
for d in ~/.cache/dissidia-vk/src ~/dissidia/*/vk_cache/src /tmp/shc_*/src; do [ -d "$d" ] && cp -n "$d"/*.glsl "$T/host/" 2>/dev/null; done
add host
for h in 192.168.2.131 deck; do
  mkdir -p "$T/$h"
  timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=8 "$h" 'for d in ~/.cache/dissidia-vk/src ~/dissidia/*/vk_cache/src ~/dissidia/*/*/vk_cache/src; do [ -d "$d" ] && tar -C "$d" -cf - . ; done' 2>/dev/null | tar -C "$T/$h" -xf - 2>/dev/null
  add "$h"
done
mkdir -p "$T/laptop"
timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=8 schindler@192.168.2.55 'cd "%LOCALAPPDATA%\dissidia-vk\src" 2>nul && tar -cf - . || (cd "%USERPROFILE%\.cache\dissidia-vk\src" && tar -cf - .)' 2>/dev/null | tar -C "$T/laptop" -xf - 2>/dev/null
add laptop
SER=$(adb devices 2>/dev/null | awk '/c2aba36c/ && $2=="device" {print $1; exit}')
mkdir -p "$T/rp6"
for b in /sdcard/Android/media /sdcard/Android/data; do   # newer installs keep the game folder under media/
  [ -n "$SER" ] && [ -z "$(ls "$T/rp6")" ] && adb -s "$SER" pull "$b/org.psprecomp.game/game/vk_cache/src/." "$T/rp6" >/dev/null 2>&1
  [ -n "$SER" ] && [ -z "$(ls "$T/rp6")" ] && adb -s "$SER" pull "$b/org.psprecomp.game/files/game/vk_cache/src/." "$T/rp6" >/dev/null 2>&1
done
add rp6
python3 -I "$ROOT/android/sync_shader_list.py" "${args[@]}"
