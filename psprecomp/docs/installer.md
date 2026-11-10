# Installer, releases and updates (design)

Status: **milestones 1 and 2 done and verified on a machine with no Rust installed** (runtime split into a prebuilt core + a link project; `psp_build.py` runs the whole build from the SDK). **Milestone 3 (toolchain bundle): Linux built and verified with the system compiler hidden; Windows cross-build not yet run.** The rest is planned below.

## The constraint

The game's code is translated to C++ from the player's **own disc** and compiled on their machine; that output is derived from the
game's code and cannot be shipped. So a release cannot contain a finished game. It contains everything that is *not* game-derived,
prebuilt, and a wizard that does the game-specific part on the player's machine:

| Shipped (prebuilt, per platform) | Done on the player's machine |
|---|---|
| the runtime core `libpsprecomp_core.a` + headers + `sdk/CMakeLists.txt` | read the ISO/CSO, extract files, decrypt the EBOOT |
| the recompiler binary `psprecomp` (Rust, prebuilt: no Rust needed) | analysis (rebuilt from the shipped metadata and the player's files, hash-checked) |
| the shipped analysis metadata `analysis/*.json.gz`, `game.toml`, overlay tomls | translate to C++ (`psprecomp recompile`) |
| a portable C++ toolchain (clang + linker + cmake/ninja) and the SDL2 / FreeType / libavcodec libraries | compile the generated C++ (the slow part: tens of minutes) and link it with the core |
| Python (embedded on Windows) and the tools (`setup_from_iso.py`, `pipeline` steps) | |
| the wizard / launcher (below) | |

Decisions (user, 2026-11-10): bundle a portable toolchain (no system installs); the wizard uses the runtime's own SDL screens (the same
game-styled menu as the settings, controller-friendly); Android later, built on a PC and copied over; updates come from GitHub Releases
of the fork.

## Milestones

1. **Core + link project** (done). `runtime/CMakeLists.txt` builds `psprecomp_core` (static library of everything except `main.cpp`) and
   the dev executable from it; `cmake --install <build> --component sdk` writes the SDK (`lib/`, `include/`, `src/main.cpp`,
   `CMakeLists.txt` from `runtime/sdk/`). `runtime/sdk/CMakeLists.txt` builds a game from a recompiled output directory and the
   SDK. Verified by building a game from the installed SDK (see "Verification").
2. **Build step as one command** (done): `games/dissidia/tools/psp_build.py <workdir> --sdk <sdk> --psprecomp <bin> [--toolchain <dir>] --progress`. Verified 2026-11-10 on the desktop (no Rust, no repo build): hydrates the shipped analysis against the player's files, recompiles, builds the overlays, configures from the SDK and compiles; the result is the same size as the dev build and boots. Original plan: a script/tool `psp-build` that runs extract -> analysis -> recompile -> compile -> link using the SDK,
   resumable (the existing `pipeline.sh` stamps), progress as machine-readable lines (`STEP n/m`, `PROGRESS percent`) for the wizard.
   Replace the bash with Python (or C++) so Windows needs no bash.
3. **Toolchain bundle** (Linux done 2026-11-10). `toolchain/build_bundle.py <platform> --out DIR` builds the bundle from pinned,
   sha256-checked inputs (`toolchain/versions.json`): zig 0.15.2 (clang 20 + lld + libc++ + libc stubs for any glibc), cmake, ninja,
   and static zlib, FreeType, SDL2 and libavcodec (ATRAC3, ATRAC3+, H.264 decoders only) built with that zig. Layout: `zig/`,
   `cmake/`, `bin/` (ninja plus the launchers `zig-cc`, `zig-c++`, `zig-ar`, `zig-ranlib` from `toolchain/zigwrap.c`, which bake the
   target in), `deps/`, `toolchain.cmake`, `deps.cmake`, `toolchain.json`. Why zig: one archive per platform that cross-targets
   (the Windows bundle is built from a Linux CI host) and brings its own libc++, so no system compiler is needed.
   - ABI rule: the prebuilt core and the player's generated code must come from the same compiler and C++ library. `toolchain.json`
     (`bundle_version`, `platform`, `zig_target`, versions) is installed into the SDK next to the core, and `psp_build.py` refuses an
     SDK/bundle pair that differs (and an SDK with a `toolchain.json` built with the system compiler unless `--allow-system-compiler`).
     Bump `bundle_version` in `versions.json` whenever anything in it changes.
   - Linux target is glibc 2.28 (`x86_64-linux-gnu.2.28`): the binary needs only libc, libm, libpthread, libdl. SDL2 is linked
     statically and dlopens X11/Wayland/ALSA/Pulse at run time; building it needs those libraries' development headers on the
     build host (CI), not on the player's machine. No GL library is linked (glad loads GL through SDL).
   - Runtime CMake: `PSPRECOMP_DEPS_CMAKE` (set by `toolchain.cmake`) makes `runtime/CMakeLists.txt` and `runtime/sdk/CMakeLists.txt`
     take SDL2/FreeType/AVCODEC/zlib from `deps.cmake` instead of pkg-config; without it nothing changed for developers.
   - `psp_build.py --toolchain <bundle>` uses `toolchain.cmake` with Ninja, puts the bundle's tools on PATH, keeps zig's cache in
     `<work>/.zig-cache`, links with `-s`, and wipes a build dir configured with another compiler.
   - Known limits: ffmpeg is built with `--disable-x86asm` (no nasm in the bundle; fine for 480x272 H.264, revisit if movie
     playback is slow); `sysctl` is patched out of ffmpeg's config.h (zig's glibc 2.28 stubs export it but the headers do not);
     the bundle is ~630 MB unpacked (cmake docs and zig extras can be trimmed); Windows (`x86_64-windows-gnu`, static SDL2) is
     written but not yet built or run; macOS is not covered.
   - Open for milestone 5: the core configure needs the four small generated headers of an output dir (`recomp_module.h`,
     `recomp_game_config.h`, `recomp_fingerprint.h`, `recomp.h`; binary facts and addresses, no game code) and CI has no game; the
     release workflow has to supply them (a committed header-only stub, or hydrate the shipped analysis).
4. **Wizard** (`psprecomp_setup`, SDL + the SdTheme screens): language-free, controller friendly. Pages: welcome, pick game file (ISO /
   CSO; a file browser), install folder, options (HD textures source, Japanese voices, update check), build progress with cancel/resume,
   done (Play). Repair and "rebuild" entries when already installed.
5. **Release workflow** (GitHub Actions, in the fork): per platform (Linux x86_64 AppImage-like folder, Windows x86_64 zip + installer
   exe), builds the core, the recompiler, the bundle, signs/hashes, and publishes a Release with a manifest (`manifest.json`: version,
   per-platform file + sha256 + size, minimum wizard version).
6. **Updater** (task 8): on startup the launcher (not the game) fetches the latest release manifest, shows "Update available" and
   updates the runtime files in place keeping the work folder (`disc0`, `ms0` saves, `settings.ini`, mods). `settings.ini`
   `check_updates=0` disables it; `PSPRECOMP_UPDATE_CHECK=0` too. When the generated code must be rebuilt (the recompiler or analysis
   changed) it says so and reruns the build step; otherwise only the core is swapped and relinked (fast).
7. **HD textures** (task 5) hooks in as an options page and a download step (Niosa / D012FFRT pack into `mods/textures`).
8. **Android**: runtime-only app; the PC wizard exports a game bundle (compiled `libpsprecomp_game.so` + data) to copy over. Separate task.

## Verification of milestone 1

The dev build (`pipeline.sh`) still produces the same `build/psprecomp_runtime` (it now links `psprecomp_core` + the generated
libraries in one link group). A second build from the installed SDK (`cmake -S <sdk> -B <dir> -DPSPRECOMP_OUTPUT_DIR=<workdir>/output
-DPSPRECOMP_OVERLAY_DIRS=...`) builds the game from scratch against the prebuilt core.

## Verification of milestone 3 (Linux)

On the desktop (no Rust, no ninja, no pkg-config): `build_bundle.py linux-x86_64` built the bundle and its smoke test (C++17 libc++
+ SDL2 + FreeType + zlib + ATRAC3/ATRAC3+/H.264 decoder lookup) passed. With a PATH that held no gcc/cc/c++/clang/ld/ar/make/cmake/
ninja/pkg-config, only the bundle: the core built (`psprecomp_core`), `cmake --install --component sdk` wrote the SDK, and
`psp_build.py work --sdk sdk --toolchain bundle --psprecomp bin` ran all five steps in a fresh work folder (82 s on 12 cores
including libc++, 50 MB stripped binary needing only libc/libm/libpthread/libdl). The earlier game build with zig and the system
libraries was played by the user (boots, loads the save).
