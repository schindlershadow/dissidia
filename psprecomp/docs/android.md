# Android version

The game runs on 64-bit ARM Android 10+ devices with OpenGL ES 3.0 (developed and tested on a Retroid Pocket 6: Snapdragon 8 Gen 2, Adreno 740,
114 fps in the 120 fps mode at 4x render scale in battle). Like the desktop version it is built from **your own disc image**: nothing of the
game is in the APK.

## With the setup wizard

1. Run `psprecomp_setup` on your PC (Windows or Linux) and turn on **Android version** in Options. The first time this downloads Google's
   Android NDK (about 0.7 GB, 2 GB on disk, kept in the release's `android/ndk/`); by turning the option on you accept its license
   (https://developer.android.com/studio/terms). It adds a few minutes to the install.
2. The wizard compiles the generated game for ARM with the NDK, packs it into an APK with the release's prebuilt Android runtime and signs it:
   `<install folder>/android/dissidia.apk`.
3. Connect the device (USB debugging, or Wireless debugging + `adb pair` / `adb connect`) with `adb` (Android platform-tools) in your PATH and
   choose **Install on an Android device** on the last page, or run
   `python games/dissidia/tools/android_install.py <install folder>`.
   It installs the APK and copies `disc0/` (the game), `ms0/` (saves), `voice_jp/`, `movie_jp/` and `mods/` to the app's folder on the device.

Without adb: install the APK by hand (allow installing from the file manager) and copy those folders to
`<internal storage>/Android/data/org.psprecomp.game/files/game/` (a file manager that can write there, or a PC over USB; on Android 11+ the
folder only exists after the app was started once). Then make sure the app can write there (saves, settings).

Command line (what the wizard runs): `psp_build.py <install folder> --sdk sdk --toolchain toolchain --psprecomp bin/psprecomp
--game game --android android` (add `--progress` for machine-readable lines).

The launcher icon is original artwork (a crystal); the setup app and the PC wizard have their own variant (teal, with a download badge: `android/icon/make_installer_icon.py`, `installer_512.png`, `setup_res/`) so they are not mistaken for the game, drawn by `android/icon/make_icon.py` (Pillow); `android/icon/icon_512.png` is the full-size version, `make_skeleton.py` compiles the sized copies into the APK. The desktop uses the same artwork: `games/dissidia/icon/` (icon.png for the Linux menu entry, icon.ico for the Windows Start Menu shortcut, both written next to the install; `embed_icon.py` regenerates them and the 64x64 window icon in `runtime/include/psp_app_icon_dissidia.h`, set with `psp_set_window_icon` for the game window and the wizard).

## Building on the phone (the setup app)

`DissidiaRecomp-Setup-<version>-android.apk` (a release asset) is an Android app that builds the game **on the phone** from your own disc image and installs it.
Nothing but the APK is downloaded, and no PC or terminal is involved.

1. Install the setup APK (allow your browser or file manager to install apps once) and open "Dissidia Recomp Setup". The first start unpacks its tools (about a minute).
2. **Build the game** explains what to pick, then opens Android's own file picker: choose your disc image (`.iso`/`.cso`) anywhere (SD card, Download...). The picker hands the
   app an open file, so no storage permission is needed to read it. "Use the simple browser instead" (on the page shown while the picker is open) is a built-in fallback (needs "all files access").
3. The app asks for "all files access" once (the game reads its files from the shared folder `<storage>/Android/media/org.psprecomp.game/game`; since Android 11 no other app
   may write to `Android/data`). Switch it on in the settings page Android opens; back in the app it continues by itself.
4. **Options** (made from the shared extras list, `setup/extras.cpp`: an extra added for the PC wizard appears here too): **HD textures** (Dissidia Recomp HD, about 1.3 GB, downloaded by the app itself, then unpacked into the
   game's packs folder; finished downloads are kept if you retry), **Add your own texture pack** (a .zip picked with Android's file picker, added next to the other packs; the game's Settings > Graphics > Texture Pack switches between them), **Japanese voices (undub)** and **Bartz: Dissidia 1 moves** (each asks for its second disc image with the same
   picker) and the update check. Later, **Change extras** on the start page opens the same page for the built game and applies it without building again.
5. **Start building** extracts and decrypts the disc, translates the game to C++ and compiles it (about 5 minutes on a Snapdragon 8 Gen 2), copies the files, then
   **Install the game** asks Android to install it (the app asks for "install unknown apps" once, then continues by itself) and **Play now** starts it.

The pages are native Android views (`android/setup/java/org/psprecomp/setup/`, no SDL): a night-sky backdrop with a slowly turning crystal, cards in the game's gold-framed menu
style (Cinzel / Noto Sans from `runtime/assets/fonts`, packed as APK assets), animated page changes. Portrait and landscape (landscape: the header beside the list, like the game's
menus; the activity handles rotation itself, so a running build is not disturbed). Touch, keyboard or a game controller: d-pad / left stick move the focus, A chooses, B goes
back, left / right flip a switch or a choice, Y opens Logs (on the build page: the log size), L / R page the log; button hints replace the version in the top bar while a
controller is used. During the build the page shows the stage, step, a per-step track, elapsed time and ETA and a **live log** (the tools' output as it arrives, errors red,
warnings amber, the build's own lines gold; it follows the newest line until scrolled). A foreground service (`BuildService`, type dataSync) keeps the build going with a
progress notification when another app is on screen, and notifies when it ends while the app is in the background.

Logs: every run appends to `files/work/setup.log`. A failed run shows the reason and its last output lines, with **Share the log** (share sheet, through `ShareProvider`, read only)
and **Save the log** (Android's "save as" picker). **Logs and reports** on the start page lists the build log, the game's `last_run.log` (when readable: "all files access"),
the updater log and the setup app's own crash reports (an uncaught exception writes `files/crash/crash-<time>.txt` with the device, versions, stack trace and the last
build lines; the next start offers to share or save it once); each opens in a viewer with Share and Save.

Code: the Java pages drive the flow (`SetupActivity`) and run plans step by step (`BuildRunner`, downloads with `Downloader`); the install logic is the C++ of the desktop
wizard (`setup/install.cpp`: `plan_install_android`, `plan_rebuild_android`, `plan_extras_android`, `feed_line` progress, `install.ini`), with `setup/proc.cpp` running the
tools in their own process group and `setup/payload.cpp` unpacking the payload, reached through JNI (`setup/android_engine.cpp`, class `Engine`; built as `libsetupcore.so`
by `android/build_setup_app.py`). Test switch: a file `files/ui_demo` (made with `run-as` on a debuggable build) replaces the build by a plan that only prints build-like
output (`fail` in it makes the build step fail): the pages can be checked where the tools cannot run, e.g. the x86_64 emulator, whose arm64 translation does not load
the musl Python.

How it works: the app ships, as `lib*.so` in its native-library folder (the only place an app may run files from), a static aarch64 **zig** (clang + lld in one binary; it
compiles the game against the NDK's headers and libc++, linking with `zig ld.lld --threads=1`), the recompiler and pspdecrypt (static musl), and python-build-standalone
(musl; its interpreter is patched to a link the app makes to Alpine's musl loader). The rest (Python's library, zig's headers, the NDK sysroot, the prebuilt Android core and libraries,
the game's tools) is one zip, `libpayload.so`, unpacked on the first start. `games/dissidia/tools/psp_build.py --android` runs unchanged on the phone
(`android_build.py` `device_build`). Built by `release/build_android_app.py` (CI job `android-app`); pinned inputs: `android/versions.json` ("tools"), `toolchain/versions.json`.
Sources: the app `android/setup/` (manifest, Java pages, theme), its engine `setup/android_engine.cpp` + `setup/install.cpp`.

Status (2026-11-13): verified on a Retroid Pocket 6 (Android 13): extract, build, copy, install and the installed game booting, driven through the app's touch UI. Not yet
verified: a first-ever install on a phone with no earlier game folder (the shared-folder fallback in `runtime/src/psp_android.cpp` needs a core built after that change),
and the CI job itself.

### Background update notifications

`android/setup/java/.../UpdateCheckService.java` is a JobScheduler job (daily, only with a network, kept across reboots; scheduled when the app starts) that fetches the latest release's
`manifest.json` and posts a notification when a newer setup app exists, or when the game built on the phone is out of date (the installed app builds it differently: "Update the game"
on the start page). It needs the notification permission (asked once on Android 13+) and is off when **Check for updates** is off in Options (`check_updates=0` in `install.ini`).
It only hints: nothing is downloaded, and tapping the notification opens the app, whose own check verifies the manifest's signature before offering anything. Each update is
announced once. Test: put a `manifest_url.txt` (a `file:` URL of a manifest with a higher version) into the app's files folder and run
`adb shell cmd jobscheduler run -f org.psprecomp.setup 4711`.

### Updates of the setup app

The app checks `manifest.json` of the latest GitHub release (the same signed manifest as the PC updater; entry `platforms.android-aarch64`) at start (silently; only the start
screen shows an offer) and from **Check for updates**. The download goes through Android's own HTTPS stack (`Downloader`: the bundled Python is musl and has no DNS
configuration or CA store on Android); `games/dissidia/tools/update.py android-check` verifies the manifest's Ed25519 signature (`game/update_key.pub`, the committed key) and compares
versions, `android-verify` checks the APK's size and sha256 (a mismatch deletes the file), then the PackageInstaller installs it (Android asks to confirm; the new APK has a higher
`versionCode`, derived from the release version by `make_skeleton.py --version`, and the same signing key, so the app and its unpacked work folder stay). The APK's contents are
fingerprinted (`android/build_fingerprint.txt`: recompiler, analysis, tools, core, libraries, zig; also in the manifest as `build_fingerprint`): when it differs from the fingerprint
stored with the last build, the start screen offers **Update the game** (translate + compile + copy again from the files already on the phone, no game file needed).
Debug switch: a file `manifest_url.txt` in the app's files folder replaces the manifest URL (the signature must still verify). CI: `manifest` job builds and signs the manifest after
the `build` and `android-app` jobs.

Status (2026-11-13): tested on the RP6 against a local server with a throwaway key: silent check, offer, download, verify, install 0.1.0 -> 0.2.0, then the rebuild prompt and rebuild.
Not yet run: the GitHub workflow, and an update against a real release.

## On the device

- **Touch controls**: shown after the first touch (Auto), hidden when a controller button is pressed; Settings > Controls > Touch Controls
  (Auto / On / Off). The menu button at the top opens the Settings menu; a built-in or Bluetooth pad works as usual.
- **Customising the touch controls**: Settings > Controls > Edit Touch Controls opens a layout editor drawn over the game. Drag any control (stick, d-pad,
  Triangle / Circle / Cross / Square, L, R, Start, Select, menu) to move it; tap one to select it, then change its **size** and **opacity**, or
  **hide** it (a hidden control ignores touches). **Add combo** creates a round button that presses several PSP buttons at once (pick any of Up, Down,
  Left, Right, Triangle, Circle, Cross, Square, L, R, Start, Select: for example L+R, or R+Triangle); for a combo you can also choose
  **Hold: toggle** (a tap presses it and keeps it held, the next tap releases it; shown with a double ring), change its buttons or delete it.
  The picker shows the buttons as shapes (arrows for the d-pad, the four symbols, L, R, a ringed play triangle for Start, two bars for Select), and a combo button shows the same shapes. **Stick: Fixed / Floating** (also in the editor's top row): floating means a touch on any empty part of the screen starts the analog stick right there and it disappears when you let go (the fixed stick is hidden). A pressed control is drawn larger. The d-pad is a single cross: a finger that went down on it keeps steering it even when it slides outside, the centre is a dead zone, and straight directions have wide zones (diagonals need the thumb clearly between two arms).
  **The right (camera) stick** is always floating: touch any empty part of the **right half** of the screen and it starts right there (drag to turn the camera; it disappears when you let go). A floating left stick only starts on the **left half**, so the two sticks never fight over a touch. A control under your finger (a face button, L / R, ...) always wins over a stick.
  **Reset** restores the default layout, **Done** saves. The layout is `touch_layout.txt` in the game folder (one line per control; positions are
  fractions of the window, so it survives a change of screen size).
- **Tall / square screens** (a folding phone's inner screen; any window whose width is at most about 1.15 x its height): while the touch
  controls are not Off and **Settings > Display > Tall Screen** is On (the row is shown only on such a screen; saved as `tall_screen`,
  `PSPRECOMP_TALL_SCREEN`; Off gives the 16:9 picture in the middle and the normal touch layout), the picture **fills the whole screen**, like Ultrawide does for wide screens but vertically. In battle (from the
  moment the fight runs until it is over: summons and the pause menu included) the game's own 480x272 picture is in the middle of the
  screen, full width, and the 3D scene goes on above and below it; the HUD is moved to the edges: the portraits, HP / Bravery bars and
  gauges to the bottom edge, the buff / EX / assist icons, multipliers and move names to the top (under L / R); the lock-on ring stays
  on the opponent. The game's glow is carried into the extra rows (mirrored from the picture's edge) so they are not darker, and
  full-screen darkening (summons, EX moves) covers the whole screen. Menus and other 2D screens are at the top of the screen (black
  below); the Settings screen itself fills the whole screen (its lists grow taller). The controls get their own layout, like a controller: L, Select, menu, Start and R along the top edge; the d-pad and the stick
  side by side on the left and the face buttons on the right, just above the HUD strip at the bottom. This layout is edited and saved
  separately (`touch_layout_tall.txt`; Reset in the editor brings back the defaults): folding or unfolding the phone switches between the
  two layouts. With Touch Controls set to Off the picture stays 16:9 in the middle; `PSPRECOMP_TALL_FILL=0` keeps the tall layout but
  shows only the 480x272 picture; `PSPRECOMP_TALL_HUD=0` leaves the HUD on the centred picture (needed by `battle_test.py`, which finds
  the fight by the HUD in the 480x272 capture). Soft Focus is skipped on these screens. On any screen the controls are sized from the
  height but never from more than 0.625 x the width, so they do not grow out of proportion on 4:3 tablets (16:10 and wider screens are
  unchanged).
- **Frame rate / render scale**: Settings > Display (frame rate) and Settings > Graphics (render scale). 30 fps is the PSP's own rate; the 120 fps mode needs a fast device. Rooted devices can set the
  CPU governor to `performance` for steadier clocks.
- **GPU drivers (Adreno phones)**: the Settings > Drivers tab (shown only on devices that can use it: an Adreno GPU). **GPU Driver** picks the Vulkan driver (System default, or a downloaded
  one); **Download Driver** loads a live list of community Mesa Turnip builds (GitHub releases of `K11MCH1/AdrenoToolsDrivers`), Cross downloads and
  installs the one shown and selects it; **Delete Driver** removes the selected one. Takes effect at the next start and needs Graphics API: Vulkan.
  A driver that crashes while Vulkan starts is skipped on the next start. Drivers are stored in the app's private storage (not the game folder).
  Ray-Traced Shadows needs a driver with `VK_KHR_ray_query` (the Adreno 740's own driver has it).
- **Preparing graphics**: with Graphics API: Vulkan the game compiles its shaders for the phone once. After a first start or an update a progress
  screen ("Preparing graphics", a bar and a count) runs before the game; later starts skip it. The shaders are the pack in the APK
  (`assets/shaders.pack`, built from `android/shaders/`); the compiled results are the SPIR-V cache in `<game folder>/vk_cache`. Anything the pack
  does not cover is still compiled when first needed (a short hitch) and recorded for the next build.
- **Debug flags**: `<game folder>/env.txt` with `KEY=VALUE` lines sets `PSPRECOMP_*` variables (for example `PSPRECOMP_PROF=1`); the log is
  `<game folder>/last_run.log` and logcat tag `psprecomp`.
- Updating: install the new APK over the old one (same key, so saves and settings stay); rebuild first when the release says the game changed.

## How it is put together

See [installer.md](installer.md), "Android progress": the renderer talks OpenGL ES through `runtime/include/psp_gl.h`; the release ships the prebuilt
Android core and libraries (SDL2, FreeType, FFmpeg decoders) built with NDK r27c, an unsigned APK skeleton (manifest + SDL's Java activity + `android/game_java`: the save manager's document picker) and a
pure-Python packer/signer (APK Signature Scheme v2, `android/apk_pack.py`, a public debug key), so the player's machine needs only the NDK compiler.
Every native library is linked for 16 KB pages (`ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES`, i.e. `-z max-page-size=16384`; the on-device link
passes it to `ld.lld` directly), so the game loads on devices running a 16 KB-page kernel and debuggable builds show no "isn't 16 KB
compatible" dialog; `apk_pack.py` warns about any `lib/*.so` whose LOAD segments are aligned below 16 KB.

## Keeping the shader list complete

The game records every shader source it asks for in `<game folder>/vk_cache/src/` (Android, always; `PSPRECOMP_VK_RECORD=1` elsewhere). Before every
Android build `android/sync_shader_list.py` (called by `android/build_game_and.sh`) pulls the recorded sources from a phone reachable over adb into
`android/shaders/` (new ones only; it does nothing when no phone is connected), and `android/pack_shaders.py` packs them into `shaders.pack`, which the
APK carries. To grow the list, play or run `games/dissidia/tools/android_battle.py` with the settings you want covered (ray-traced shadows, SSAO,
reflections...) and build again. `sync_shader_list.py --prune` drops entries the phone did not record (clear `vk_cache/src` on the phone first).

