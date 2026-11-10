# Installer, releases and updates (design)

Status: **milestones 1 and 2 done and verified on a machine with no Rust installed** (runtime split into a prebuilt core + a link project; `psp_build.py` runs the whole build from the SDK). The rest is planned below.

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
3. **Toolchain bundle** (next). ABI warning found while planning: the prebuilt core and the player's generated code must come from the same compiler and C++ library (gcc/libstdc++ and clang/libc++ do not link together), so the release's core is built by CI with the very toolchain it ships. Plan: pinned clang + lld + cmake + ninja (zig cc is the candidate: one small archive per platform, cross-targets,
   bundled libc++/mingw headers) and SDL2 / FreeType / libavcodec (static or shared, with `.pc` files for the SDK's pkg-config
   lookups). A script builds the bundle per platform and checks its hash.
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
