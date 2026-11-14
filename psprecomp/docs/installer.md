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
4. **Wizard** (`setup/`, program `DissidiaRecomp`; first version built and driven on Linux 2026-11-10). Requirement (user): it
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
   - **Change extras** (start page of an installed game): the Extras page again, applied without a rebuild (`plan_extras`): downloads / swaps / removes the HD texture pack, adds the Japanese voices, builds the Android version once.
   - **Options** (tabs like the game's Settings menu, L / R switch: Extras = the features and Start installing, General = update check and install folder): check for updates; HD textures (LiinkPK's hand-retouched pack, listed first, or the AI upscaled pack; Left/Right cycles, the help line credits the selected pack); Japanese voices (undub; asks for the undub disc image first); Android
     version; **Import saves** (cycles Off / each PPSSPP or memory-stick SAVEDATA folder found with Dissidia 012 saves, found by
     `psp_savemgr::detect_sources`; it adds the last install step, `DissidiaRecomp --import-saves FROM --dir WORK`, which copies the
     saves under the installed game's disc ID, re-signed, and never replaces a save that already exists in the install; verified
     2026-11-10 with a headless install from the US ISO, `--headless ... --saves DIR`). A release declares which of the optional features it implements in `game/features.ini` (`hd_textures=1`,
     `jp_audio=1`, `android=1`); until a feature exists its row shows "Not yet" and cannot be switched on. The commands the
     wizard runs for them are fixed in `install.cpp` (`tools/fetch_textures.py <work> --pack liink|ai`, `tools/jp_audio.py --iso <undub>
     --work <work>`, `psp_build.py ... --android`), so those tasks only have to provide the scripts and flip the line.
   - Release layout the wizard expects (see `setup/install.h`): `DissidiaRecomp`, `bin/psprecomp`, `toolchain/`, `sdk/`,
     `game/` (= `games/dissidia`), `python/` on Windows. It remembers the install in the user's pref dir (`install.ini`).
   - Command line: `--play [DIR]` starts the installed game; `--headless --iso F --dir D` runs the install steps without a window
     (used for tests); `--root DIR`; `--screenshots DIR` renders every page to PPM without a display (look checks).
   - Not yet: the update check (a welcome-page row is reserved), Windows run, per-step failure hints, an installer exe / desktop
     shortcut.
5. **Release workflow** (written 2026-11-10; the local run below works, the GitHub Actions run itself has not been run yet because nothing is pushed).
   - `release/build_release.py --platform P --version V --work DIR` is the whole build for one platform on a Linux host: bundle
     (`toolchain/build_bundle.py`) -> header stub -> core + SDK -> wizard -> `release/assemble.py` -> `DissidiaRecomp-V-P.tar.xz`
     (Linux) / `.zip` (Windows) + `.sha256`. Windows is cross-built; `build_bundle.py` keeps `<bundle>.host/` (host-runnable zig,
     cmake, ninja + `toolchain-cross.cmake`) for that, and the Windows recompiler comes from a Windows runner.
   - **Header stub** (the open question of milestone 3, solved): `psprecomp recompile <shipped analysis> --config game.toml
     --headers-only` writes the four small generated headers (`recomp.h`, `recomp_module.h`, `recomp_game_config.h`,
     `recomp_fingerprint.h`) plus a CMakeLists defining `psp::recomp` as an empty interface target, from the shipped analysis
     metadata alone (no game bytes; `data_b64` is now optional in the JSON and a normal `recompile` refuses an unhydrated file).
     The three non-fingerprint headers are byte-identical to the ones of a real output (checked). The core is then built with
     `PSPRECOMP_OUTPUT_DIR=<stub>`, so CI needs no game.
   - `.github/workflows/release.yml`: on a `v*` tag (or by hand for artifacts only) a Windows job builds the recompiler, an Ubuntu
     job installs SDL2's build headers, builds both platforms with `build_release.py`, writes `manifest.json`
     (`release/make_manifest.py`) and a `publish` job runs `gh release create` with the archives and the manifest.
   - **Release contents** (`assemble.py`): the wizard, `bin/psprecomp`, the trimmed bundle, the SDK, `game/` (whitelist: analysis
     metadata, game.toml, overlay tomls, features.ini, fx_shaders, `tools/*.py|json|txt` without tests), embedded Python on Windows
     (pinned in `toolchain/versions.json`; its `._pth` gets `..\game\tools` added), `licenses/`, `release.json` (version,
     platform, bundle identity, sha256 of every file, `rebuild_fingerprint`, `relink_fingerprint`). `assemble.py` refuses an SDK
     built with a different bundle than the one it ships.
   - **manifest.json** (what the updater downloads first): per platform asset name, url, sha256, size, `bundle_version` and the two
     fingerprints from the archive's `release.json`; plus `min_wizard_version`.
   - Licenses: pspdecrypt is GPL-3.0 and ships as a separate executable (source and license pinned in the bundle), OpenSSL 3.0
     (Apache-2.0), SDL2 and zlib (zlib license), FreeType (FTL), FFmpeg (LGPL 2.1+; built without GPL parts, linked statically into the
     game the player builds: `licenses/THIRD-PARTY-NOTICES.txt` (from `release/third_party_notices.py`) gives the pinned source URL + sha256, the configure flags, the two build-time edits and a written offer; the license texts are copied into `toolchain/licenses/` by `build_bundle.py` and into the release's `licenses/` by `assemble.py`; the player links the game from the shipped static libraries, so the library can be replaced), portable-file-dialogs (WTFPL).
6. **Updater** (task 8; built 2026-11-10, update.py verified against a local file:// manifest, the wizard screens are not clicked through yet).
   - `game/tools/update.py check|apply --root <release folder>` does the work (Python stdlib, so Windows uses the embedded one).
     `check` fetches `manifest.json` (URL written into `release.json` at assemble time: the fork's latest release,
     `PSPRECOMP_MANIFEST_URL` overrides) and prints one line (`UPDATE version=.. current=.. size=.. rebuild=0|1 relink=0|1`,
     `NOUPDATE ..` or `ERROR ..`) and always exits 0, with an 8 s network timeout, so a failed check never stops the game.
     `apply` downloads the archive for the platform with progress lines, verifies its sha256, unpacks it to `<root>/.update`,
     replaces only files whose hash differs (a running exe is renamed to `.old` first on Windows), deletes files the new release
     dropped and writes `release.json` last (an interrupted update is just run again).
   - What follows an update: `rebuild=1` when the recompiler, analysis, overlay tomls, game tools or the toolchain bundle changed (the
     generated code must be regenerated and compiled: `psp_build.py`, whose stamps skip whatever is still valid), `relink=1` when only
     the prebuilt core (`sdk/`) changed (the same command relinks in seconds), neither: files only. The work folder (disc0, saves,
     settings.ini, mods) is outside the release folder and untouched. A different toolchain bundle is guarded by the SDK
     `toolchain.json` check.
   - In the wizard: with an install present it checks at start (background, 15 s cap) and shows an "Update available" page (Update
     now / Not now) and, from the start page, "Check for updates" on demand. `DissidiaRecomp --launch` is the shortcut mode for
     starting the game: check, then play at once when there is nothing to do.
   - **Opt-out**: `check_updates=0` in the work folder's `settings.ini` (the wizard writes the player's choice from the Options
     page at install time), or `PSPRECOMP_UPDATE_CHECK=0`. In-game: Settings > System > Check for Updates writes the same `check_updates=` line (built 2026-11-10). Signed manifests: see below. Not yet: delta downloads, wizard self-restart after
     an update.
   - **Signed manifests** (built 2026-11-10; `games/dissidia/tools/ed25519.py` is pure-Python RFC 8032, checked against the RFC vector and
     against OpenSSL both ways). If a release ships `game/update_key.pub` (the committed `games/dissidia/update_key.pub`), `update.py`
     fetches `<manifest url>.sig` and refuses a manifest with a missing or wrong signature, so a hijacked download host cannot push
     an update (the archive's sha256 is inside the signed manifest). Without the key file only the sha256 protects an update.
     One-time setup, which needs the owner: `python3 release/sign_manifest.py keygen`, commit the public value as
     `games/dissidia/update_key.pub`, store the seed as the GitHub secret `RELEASE_SIGNING_KEY`; the workflow then signs
     (`release/sign_manifest.py sign`) and fails if the key is committed but the secret is not. Rotating the key needs one release
     signed with the old key that ships the new `update_key.pub`. Tests: `games/dissidia/tools/test_update_signature.py`.
7. **HD textures** (task 5) hooks in as an options page and a download step (Niosa / D012FFRT pack into `mods/textures`).
8. **Android**: runtime-only app; the PC wizard exports a game bundle (compiled `libpsprecomp_game.so` + data) to copy over. Separate task.
   Findings: zig can target `aarch64-linux-android`, so the same bundle idea cross-compiles the generated code and the core on the PC
   (the player's PC, not the phone; no on-device compiler). The large items are not the toolchain: the renderer uses desktop
   OpenGL 3.3 core (glad) and would need a GLES 3 path, SDL2 needs its Java activity and an APK (Gradle + Android SDK for packaging, or
   a prebuilt APK shell that loads `libpsprecomp_game.so` from app storage so no SDK is needed on the player's PC), touch/gamepad
   input, and a bionic port of the core's POSIX use (overlaps with task 19). Order: after Windows.

### Android progress (task 20)

Milestone 1, hello APK (2026-11-10): `android/build_hello.py` builds SDL2 (shared, from the pinned 2.32.10 source) and a tiny `libmain.so`
(`android/hello/hello.c`: GLES 3.0 context, background colour follows touch, flash on pad buttons, GL strings / drawable size / pad
and touch events in logcat) for arm64-v8a with the Android NDK 27 (the NDK is needed: zig has no bionic headers), packs SDL's Java
activity (javac + d8), aapt2, zipalign and a debug-signed apk. Tools live outside the repo (desktop `~/dissidia/android/{sdk,jdk,sdl2src}`:
cmdline-tools sdkmanager `build-tools;34.0.0`, `platforms;android-34`, `ndk;27.2.12479018`; Corretto JDK 17). Built: `hello.apk`, 2.4 MB;
run on the Retroid Pocket 6 (Adreno 740, OpenGL ES 3.2, 1920x1080, built-in "Xbox Wireless Controller" pad, Android 13) over wireless adb: window, GLES3 context and pad detection OK (2026-11-10); touch + button events awaiting the user. 
Milestone 2, the game on the device (2026-11-10): the renderer goes through `runtime/include/psp_gl.h` (glad on desktop, `<GLES3/gl32.h>` on Android:
`glDepthRange`/`glClearDepth` to the `f` forms, `glMultiDrawArrays` as a loop, `glGetBufferSubData` through a mapping, point size through a
`u_psize` uniform written to `gl_PointSize`, GLSL `#version 300 es` + precisions via `PSP_GLSL_VERSION`, no per-texture LOD bias, no timer queries / GL call
statistics). The core and the player's generated game compile with the NDK unchanged otherwise (the Windows-port shims covered the POSIX use).
`android/build_deps.py` builds SDL2 (shared), FreeType and FFmpeg (no asm: the NEON code is not PIC) for arm64 with the NDK; the SDK link project
builds `libmain.so` (SHARED, `SDL_main` in `src/main.cpp`) when `ANDROID` is set; `build_hello.py --main-so ... --package org.psprecomp.game` packs the APK.
`psp_android.cpp`: work folder = `<external storage>/Android/data/org.psprecomp.game/files/game` (the desktop work folder: disc0/, ms0/, mods/, voice_jp/ ...),
stderr to logcat tag `psprecomp` and `last_run.log` there. Android 13 `adb push` into Android/data fails on nested folders: copy with
`tar cf - disc0 | adb shell "tar xf - -C <dir>"`. Performance on the RP6 (Snapdragon 8 Gen 2, Adreno 740), measured with `simpleperf` (the APK is debuggable: `simpleperf record --app org.psprecomp.game -g`, report on the build machine with the NDK's host simpleperf and `--symfs` pointing at the unstripped libmain.so): build at API 29 (below it thread_local is emulated, ~15%); texture-cache hash on four lanes (was 20% of a battle); no shared-ring atomic per function entry on Android; the CPU governor `walt` idles the cores low, so `performance` (root: `echo performance > /sys/devices/system/cpu/cpuN/cpufreq/scaling_governor`) matters. Result: 114 fps in the 120 fps mode at 4x render scale in battle (user, 2026-11-10). `-ftls-model=initial-exec` does NOT work (bionic: "dlopen failed: TLS symbol (null)" for a dlopen()ed library); remaining cost: the dynamic TLS resolver (~8%) and the per-bind hash. Wireless adb port changes on every enable: find it by probing ports 30000-46000 on the phone's IP.
Touch controls (`runtime/src/psp_touch.cpp`, 2026-11-10): fingers become synthetic SDL controller events (`which = PSP_TOUCH_PAD_ID`), so the game, the settings menu and the dialogs need no touch code; a GL pass at the end of `present_blit` draws translucent controls over the whole window (own program/VAO, GL state saved and restored, the GE's cached state invalidated after). Layout: left stick bottom-left with a d-pad above it, face buttons bottom-right, L / R top corners, Select / Start bottom centre, a menu button (Guide: opens Settings) top centre. Settings > Controls > Touch Controls (Android builds only; Auto / On / Off, `touch=` in settings.ini, `PSPRECOMP_TOUCH`): Auto shows them after the first touch (that touch presses nothing) and hides them when a real pad button is pressed; desktop default Off. Sliding between face buttons / d-pad directions works; multi-touch. Layout editor (same file): controls are a list (`Ctl`: position / size / opacity / on-off, a button set for combinations, toggle-hold), saved in `touch_layout.txt`; the editor is drawn and hit-tested in the same overlay with a built-in 5x7 bitmap font (no font files), opened from Settings > Controls > Edit Touch Controls (or `PSPRECOMP_TOUCH_EDIT=1` at start, for tests); in the editor the fingers produce no pad events.
Wizard part (2026-11-10): the release carries `android/` (built by `release/build_android_sdk.py`, wired into `build_release.py --android-sdk` and
`assemble.py --android-part`, which also sets `android=1` in game/features.ini): prebuilt Android core + link project, SDL2 / FreeType / FFmpeg for arm64, the APK
skeleton and `apk_pack.py` (pure-Python zip append + APK Signature Scheme v2 with RSA from the standard library, verified with Google's apksigner; key =
public debug key `android/debug_key.json`), `libc++_shared.so`, `versions.json` (pinned NDK r27c, API 29) and `android.json` (what the prebuilt parts were built with,
checked against the pin). On the player's machine `psp_build.py --android` (games/dissidia/tools/android_build.py) downloads the NDK once (sha1 checked, only the
clang toolchain + build files unpacked, symlinks restored), configures the SDK link project with the NDK toolchain, compiles the generated game, strips and
packs `<work>/android/dissidia.apk`; `android_install.py` installs it with adb and streams the game folders as tar (`adb exec-in`). The wizard: Options help
explains the download and the NDK license, the Done page offers "Install on an Android device". Verified on the desktop with a PATH that has only cmake + ninja
(no JDK, no Android SDK): 13 MB APK; installed on the RP6. See android.md for the user view.
Files copied by adb belong to the shell user, which the app cannot write (settings.ini and saves silently fail): run `adb shell chmod -R a+rwX <work folder>` after copying. Result on the Retroid Pocket 6: boots, loads the save, 30 GE frames/s, ATRAC audio decodes, the uber-shader
compiles first time; picture/audio/input quality awaiting the user.
Fixes after the first look (2026-11-10): (1) no TrueType font on Android -> every sceFont string blank: `psp_font_host_files` now uses `/system/fonts/DroidSans.ttf` (+ Bold);
(2) Adreno does not honour buffer orphaning: wrapping the vertex ring with `glBufferData(NULL)` while earlier draws were still pending made them read the NEW
storage (streaks, sprites from other screens, text swapped). On ES the ring now waits (`glFinish`) at a wrap and reuses the buffer, 512 K vertices per ring
(found by bisecting: legacy renderer fine, `PSPRECOMP_GE_SYNC=1` fine, a ring that never wraps fine). (3) speed: `glBufferSubData` into the ring cost ~0.5 ms per call on Adreno (battle: upload 62 of 77 ms per frame, 11 fps); ES now defaults to the unsynchronised mapping (`PSPRECOMP_GE_RING=map`): the RP6 holds 30 fps at 4x render scale, 60 fps in the 120 fps mode at 4x (user, 2026-11-10). `<work folder>/env.txt` (KEY=VALUE lines) sets `PSPRECOMP_*` flags on Android.
`android/redeploy.sh <adb serial>` rebuilds the library + APK on the build machine and reinstalls.

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

## pspdecrypt in the bundle (bundle_version 2)

The installer needs `pspdecrypt` to decrypt the game's `EBOOT.BIN`; `setup_from_iso.py` used to build it from source with a system
compiler plus OpenSSL headers. `build_bundle.py` now builds it for both platforms with the bundle's own zig and installs it as
`<bundle>/bin/pspdecrypt[.exe]` (plus `bin/pspdecrypt-LICENSE.txt`). Inputs pinned in `toolchain/versions.json`:

- `pspdecrypt` John-K/pspdecrypt at commit `c156627db7634d395c380c0a9589130f603307fc` (archive tarball, sha256 pinned). License:
  **GPL-3.0** (`LICENSE.TXT` in the source, copied next to the binary). It is a separate executable the installer runs, not linked
  into the runtime; shipping the binary means offering the source, which is the pinned URL above.
- `openssl` 3.0.18 (Apache-2.0). Only SHA-256 and DES-CBC are used by pspdecrypt, but the real libcrypto is built (static, `no-shared
  no-asm no-threads no-tls ...`, `Configure linux-x86_64` / `mingw64` with `CC=zig-cc`) in the scratch dir and linked in; it is not
  installed into the bundle. Needs `perl` and `make` on the build host, like ffmpeg needs `make`.

The executables are self-contained: Linux needs only libc (zig target glibc 2.28), Windows imports only KERNEL32 and the `api-ms-win-crt-*`
UCRT forwarders (no mingw DLLs). `setup_from_iso.py` finds it through `PSPRECOMP_TOOLCHAIN=<bundle>` (order: `--pspdecrypt`,
`$PSPDECRYPT`, `$PSPRECOMP_TOOLCHAIN/bin/pspdecrypt[.exe]`, `<workdir>/tools/pspdecrypt`, `PATH`, then build from source).

Verification (2026-11-10, desktop): Linux with a PATH holding no compiler, `bin/pspdecrypt -o out.elf EBOOT.enc.bin` output is
byte-identical (`cmp`) to the earlier decrypted ELF made by the source-built binary. The Windows `pspdecrypt.exe` (PE32+) run under
Wine (`WINEDEBUG=-all`, command line only) produced the identical ELF as well.

## Fixes after the first desktop test (2026-11-10)

- **Menu entry**: after an install the wizard writes `~/.local/share/applications/dissidia-012.desktop` (Linux; icon = the disc's
  ICON0.PNG copied to `<workdir>/icon.png`) or a Start Menu `.lnk` (Windows, created through PowerShell's WScript.Shell). It runs
  `DissidiaRecomp --launch`, so an update check comes first. An existing install gets the entry the next time the wizard starts.
- **Window title**: a game module names its window with `psp_set_window_title()` (Dissidia: "Dissidia 012 Duodecim Final Fantasy").
- **Undub disc images made with xorriso/mkisofs**: file names longer than 8.3 live only in the Rock Ridge `NM` records
  (`PACKAGE_INFO.BIN` is `PACKAGE_.BIN` in the plain ISO 9660 tree). `setup_from_iso.Iso9660` now reads them.
- **Settings menu**: Voice Language and HD Textures rows appear when `voice_jp/` / `mods/textures/` exist next to `ms0`.
- **Saves are shared between regions** (`psp_savedata_set_shared_ids`); see games/dissidia/docs/savedata.md.


## Single-file release

Besides the archives, each release carries `DissidiaRecomp-Setup-<version>-windows-x86_64.exe` and `DissidiaRecomp-Setup-<version>-linux-x86_64`:
the wizard program with the assembled release folder appended as a zip (`release/make_single.py`, `setup/payload.{h,cpp}`, miniz).
On its first start the wizard unpacks the zip into `%LOCALAPPDATA%\psprecomp\v<version>` (Linux: `~/.local/share/psprecomp/v<version>`;
a small progress window, first start only) and runs from there; the unpacked folder contains the plain wizard, so it never unpacks
twice. `--unpack DIR` unpacks into DIR and exits (CI uses it to check the Linux file). The updater keeps working on the unpacked
folder. Downloading a newer single file unpacks next to the old one; the old folder can be deleted. Unpacked size is about 530 MB.

Naming: players see **Dissidia Recomp** (window titles, release files `DissidiaRecomp-*`, program `DissidiaRecomp[.exe]`, unpack folder
`DissidiaRecomp`). Internal names (crates, `psprecomp` recompiler binary, `PSPRECOMP_*` variables, CMake targets) are unchanged.
