# Installer, releases and updates (design)

Status: **milestones 1 and 2 done and verified on a machine with no Rust installed** (runtime split into a prebuilt core + a link project; `psp_build.py` runs the whole build from the SDK). **Milestone 3 (toolchain bundle): Linux built and verified with the system compiler hidden (user-confirmed in game); the Windows bundle builds from a Linux host, but the runtime core is not ported to Windows yet (see below).** The rest is planned below.

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
     the bundle is ~630 MB unpacked (cmake docs and zig extras can be trimmed); macOS is not covered.
   - Open for milestone 5: the core configure needs the four small generated headers of an output dir (`recomp_module.h`,
     `recomp_game_config.h`, `recomp_fingerprint.h`, `recomp.h`; binary facts and addresses, no game code) and CI has no game; the
     release workflow has to supply them (a committed header-only stub, or hydrate the shipped analysis).
4. **Wizard** (`setup/`, program `psprecomp_setup`; first version built and driven on Linux 2026-11-10). Requirement (user): it
   runs on **Windows as well as Linux**. It is a C++/SDL2 program built with the bundle for each platform, using portable code
   only (`std::filesystem`, a small process helper `setup/proc.cpp` with POSIX fork/pipes or Win32 CreateProcess + a job object so
   Cancel kills ninja/zig too), and it reuses the runtime's menu renderer (`render_menu`, `KeyboardDialog` from
   `runtime/src/hle/psp_savedata_dialog.cpp`: the settings-menu look in its generic theme; the game's own art does not exist
   yet before the disc is read). Keyboard or controller (d-pad/stick, A = select, B = back). Pages: welcome (or Play / Rebuild /
   Install again once installed), choose the game file, where to install, options, progress, done / failed.
   - **System file dialogs**: the game file, the install folder and the undub file are picked with the platform's own dialog
     (`setup/third_party/portable-file-dialogs.h`, WTFPL, pinned commit: Win32 on Windows, `zenity` or `kdialog` on Linux). Where
     there is none (no zenity/kdialog) or on request ("Use the simple browser instead") the wizard's built-in browser, which
     also has a type-a-path keyboard, is used.
   - **Install folder**: a suggested default (`~/Games/Dissidia012`, `%USERPROFILE%\Games\Dissidia012`) with a free-space check
     (about 6 GB needed), "Choose another folder...", "Use the suggested folder".
   - **Progress page**: stage name and step count, a bar, a detail line (ninja's "Compiled 183 of 474: file"), elapsed time, an ETA
     ("About 6 minutes left", from ninja's file counter during the compile, from the overall fraction elsewhere) and an explanation
     of what the current stage does in the information panel. The step weights live in `setup/install.cpp`.
   - **Options**: check for updates; HD texture pack; Japanese voices (undub; asks for the undub disc image first); Android
     version. A release declares which of the optional features it implements in `game/features.ini` (`hd_textures=1`,
     `jp_audio=1`, `android=1`); until a feature exists its row shows "Not yet" and cannot be switched on. The commands the
     wizard runs for them are fixed in `install.cpp` (`tools/fetch_textures.py <work>`, `tools/jp_audio.py --iso <undub>
     --work <work>`, `psp_build.py ... --android`), so those tasks only have to provide the scripts and flip the line.
   - Release layout the wizard expects (see `setup/install.h`): `psprecomp_setup`, `bin/psprecomp`, `toolchain/`, `sdk/`,
     `game/` (= `games/dissidia`), `python/` on Windows. It remembers the install in the user's pref dir (`install.ini`).
   - Command line: `--play [DIR]` starts the installed game; `--headless --iso F --dir D` runs the install steps without a window
     (used for tests); `--root DIR`; `--screenshots DIR` renders every page to PPM without a display (look checks).
   - Not yet: the update check (a welcome-page row is reserved), Windows run, per-step failure hints, an installer exe / desktop
     shortcut.
5. **Release workflow** (GitHub Actions, in the fork): per platform (Linux x86_64 AppImage-like folder, Windows x86_64 zip + installer
   exe), builds the core, the recompiler, the bundle, signs/hashes, and publishes a Release with a manifest (`manifest.json`: version,
   per-platform file + sha256 + size, minimum wizard version).
6. **Updater** (task 8): on startup the launcher (not the game) fetches the latest release manifest, shows "Update available" and
   updates the runtime files in place keeping the work folder (`disc0`, `ms0` saves, `settings.ini`, mods). `settings.ini`
   `check_updates=0` disables it; `PSPRECOMP_UPDATE_CHECK=0` too. When the generated code must be rebuilt (the recompiler or analysis
   changed) it says so and reruns the build step; otherwise only the core is swapped and relinked (fast).
7. **HD textures** (task 5) hooks in as an options page and a download step (Niosa / D012FFRT pack into `mods/textures`).
8. **Android**: runtime-only app; the PC wizard exports a game bundle (compiled `libpsprecomp_game.so` + data) to copy over. Separate task.
   Findings: zig can target `aarch64-linux-android`, so the same bundle idea cross-compiles the generated code and the core on the PC
   (the player's PC, not the phone; no on-device compiler). The large items are not the toolchain: the renderer uses desktop
   OpenGL 3.3 core (glad) and would need a GLES 3 path, SDL2 needs its Java activity and an APK (Gradle + Android SDK for packaging, or
   a prebuilt APK shell that loads `libpsprecomp_game.so` from app storage so no SDK is needed on the player's PC), touch/gamepad
   input, and a bionic port of the core's POSIX use (overlaps with task 19). Order: after Windows.

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

## Windows status (2026-11-10)

`build_bundle.py windows-x86_64` (run on a Linux host) produces a bundle of Windows executables (zig, cmake, ninja, the `zig-*.exe`
launchers) and static mingw `libSDL2.a`, `libfreetype.a`, `libz.a`, `libavcodec.a`/`libavutil.a`; Windows-only workarounds in the script:
zlib static target only, ffmpeg configure's `--pic-executable` link flag, the system libraries SDL2 needs listed in `deps.cmake`.

**Task 19, Windows port of the core: builds.** The POSIX calls the runtime used now go through `runtime/include/psp_platform.h`
(`psp_plat::`: guest memory via `VirtualAlloc` instead of `mmap`, `mkdir`, `pread` via overlapped `ReadFile`, `setenv`, `localtime`,
free disk space, file mtime, Winsock for the debug socket and ad-hoc UDP incl. `WSAPoll` and a non-blocking socket instead of
`MSG_DONTWAIT`; `sigaction` becomes `signal`; the `/proc` thread profiler is Linux only; `main()` uses `SDL_MAIN_HANDLED`; the VFPU
`BitScan.h` takes the compiler builtins on mingw). On Linux and macOS each shim is a thin inline over the original call.
Result: the core, the generated code, the overlays and `psprecomp_runtime.exe` (50 MB) compile and link for `x86_64-windows-gnu` with
the bundle's compiler (run through the host launchers on Linux), and the Linux build is unchanged (rebuilt and linked again).

Not verified: **no Windows binary has been run.** Open questions only a run shows: path handling (`/` vs `\\`, drive letters, case
rules in `psp_path_to_host`), GL context creation through static SDL2 on Windows, the audio sink, threads/fibers in the scheduler, `getenv`
based settings (settings.ini applies them with `_putenv_s`), Winsock UDP error codes (e.g. `WSAECONNRESET` on ICMP port unreachable).
Wine is installed on the desktop for a first look; a Windows machine is the real test.
