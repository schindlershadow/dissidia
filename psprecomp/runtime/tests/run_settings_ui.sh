#!/bin/sh
# Build and run the settings screen host test (g++ + FreeType).
# DUMP_SETTINGS=/dir writes PPM frames of the screen for eyeballing.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
fonts="${TMPDIR:-/tmp}/psp_ui_fonts.cpp"
cmake -DFONT_DIR="$here/../assets/fonts" -DOUT="$fonts" -P "$here/../assets/embed_fonts.cmake"
out="${TMPDIR:-/tmp}/test_settings_ui"
g++ -std=c++17 -O2 -Wall -Wextra -I"$here/../include" -o "$out" \
  "$here/test_settings_ui.cpp" "$here/../src/hle/psp_savedata_dialog.cpp" "$fonts" \
  $(pkg-config --cflags --libs freetype2)
"$out"
