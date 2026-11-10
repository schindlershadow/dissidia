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

## On the device

- **Touch controls**: shown after the first touch (Auto), hidden when a controller button is pressed; Settings > System > Touch Controls
  (Auto / On / Off). The menu button at the top opens the Settings menu; a built-in or Bluetooth pad works as usual.
- **Frame rate / render scale**: Settings > System. 30 fps is the PSP's own rate; the 120 fps mode needs a fast device. Rooted devices can set the
  CPU governor to `performance` for steadier clocks.
- **Debug flags**: `<game folder>/env.txt` with `KEY=VALUE` lines sets `PSPRECOMP_*` variables (for example `PSPRECOMP_PROF=1`); the log is
  `<game folder>/last_run.log` and logcat tag `psprecomp`.
- Updating: install the new APK over the old one (same key, so saves and settings stay); rebuild first when the release says the game changed.

## How it is put together

See [installer.md](installer.md), "Android progress": the renderer talks OpenGL ES through `runtime/include/psp_gl.h`; the release ships the prebuilt
Android core and libraries (SDL2, FreeType, FFmpeg decoders) built with NDK r27c, an unsigned APK skeleton (manifest + SDL's Java activity) and a
pure-Python packer/signer (APK Signature Scheme v2, `android/apk_pack.py`, a public debug key), so the player's machine needs only the NDK compiler.
