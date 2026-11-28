# The native build in detail

How the three setup steps work, the work folder, and every runtime option (window, scaling, render resolution, input, camera). Players install with the setup app ([top-level README](../../../../README.md#install)), which runs these same steps for them; the short from-source steps are in [Building from source](../../../../README.md#building-from-source), and the Steam Deck walkthrough is [steamdeck.md](steamdeck.md). Paths are relative to `psprecomp/games/dissidia/`.

## What each step does

You need your own copy of the game, a 64-bit Linux PC or a Steam Deck, about 8 GB of free disk space, and patience:
the first build takes a long time (tens of minutes to hours). The from-source steps for a Linux PC or a Mac are in the
top-level README: [Building from source](../../../../README.md#building-from-source); for the Deck, [`docs/steamdeck.md`](steamdeck.md). Steam Deck details (Steam, controls,
display, troubleshooting): [`docs/steamdeck.md`](steamdeck.md).

On a Linux PC or a Mac, `tools/install.sh <game.iso> [workdir]` does all of it in one go: the build tools (apt or Homebrew,
only what is missing), Rust (rustup), then the steps below, the tools' Python packages, and `<workdir>/play.sh`, a
launcher that runs `run.sh` with any flags you give it. `--jobs N`, `--update` (git pull first), `--no-packages`.
The Steam Deck has its own, `deck_setup.sh`. The steps by hand:

1. `setup_from_iso.py` reads the ISO/CSO itself (no tools needed), copies the files the game uses to
   `<workdir>/disc0`, decrypts `EBOOT.BIN` (it builds the small `pspdecrypt` tool for this; see the
   script's `--help` for alternatives), and creates the emulated Memory Stick `<workdir>/ms0`.
2. `pipeline.sh` builds the recompiler (Rust), rebuilds the analysis from the shipped metadata in
   `analysis/` and your own files, translates the game to C++ and compiles it with the runtime into
   `<workdir>/build/psprecomp_runtime`. Re-running it is safe and compiles only what changed. It starts as many compile jobs as the free memory allows; `--jobs 2` sets a fixed count.
3. `run.sh` starts the game with the right environment. Saves are in `<workdir>/ms0/PSP/SAVEDATA/`.
   Gamepads work out of the box (A = Cross); `--swap-ab` swaps confirm/cancel. Keyboard: arrows, X/Z/A/S,
   Q/W, Space = Start, Tab = Select. `--quick-battle` loads the save and goes straight to the Quick Battle
   (1-on-1) character select: `tools/quick_battle.py` presses the buttons through the runtime's debug socket,
   checking each screen in a screenshot first (a press the game drops in a fade is pressed again), with the
   game fast-forwarded for the trip (the debug socket's `FF`, see DEBUGGING.md), then the game is yours at its
   own speed. It needs a save in `ms0` (without one the game starts as usual).

If something fails, the scripts say what is missing. This is a work-in-progress preview: see
[Status](#status) for what does and does not work yet.

Developers: to re-analyse with Ghidra instead (needed for a different region or build), install JDK 21,
Ghidra 12.x and the ghidra-allegrex extension (versions pinned in the top-level README) and run
`pipeline.sh <workdir> --ghidra /path/to/ghidra`. The shipped `analysis/*.json.gz` files contain no game
code (function tables only; the code bytes are re-read from your disc and hash-checked), and are
regenerated with `tools/ship_analysis.py strip-all`.

## Work directory layout

```
<workdir>/
  disc0/PSP_GAME/SYSDIR/{EBOOT,BOOT}.BIN   decrypted ELF (the runtime boots this)
  disc0/PSP_GAME/USRDIR/DATA/...            PACKAGE.BIN, PACKAGE_INFO.BIN, OVL_*_APP.ELF, MODULE/, BGM/
  disc0/PSP_GAME/USRDIR/DATA/MOVIE/         movies
  ms0/PSP/{SAVEDATA,GAME}                   Memory Stick root (saves)
  EBOOT.enc.bin, EBOOT.dec.elf              encrypted / decrypted EBOOT
  OVL_{BATTLE,MENU,EXTRA}_APP.bin           flat overlay blobs (ELF offset 0x54 onward)
  eboot_image.bin                           flat EBOOT image 0x08804000..0x089DD4C0
  analysis*.json                            analysis results (analysis, _battle, _menu, _extra, _supacc)
  last_run.log                              runtime log of the last run.sh
  output/  overlays/{battle,menu,extra,supacc}/   generated C++
  objcache/                                 compiled game code kept for the next update (tools/objcache.py)
  build/psprecomp_runtime                   the runtime
  tools/pspdecrypt                          built decryptor
```

The runtime reads `PSPRECOMP_DISC0=<workdir>/disc0` and `PSPRECOMP_MS0=<workdir>/ms0` (both set by
`run.sh`). `PSPRECOMP_CROSS_MID=1` is needed when recompiling (`pipeline.sh` sets it); the runtime does not
read it. `PSPRECOMP_CLEANROOM` is not used. Optional: `PSPRECOMP_OSK_TEXT`,
`PSPRECOMP_FONT`, `PSPRECOMP_FONT_SCALE`, `PSPRECOMP_AUDIO=0`.

Look presets (`run.sh --retro`, `--crisp`; `PSPRECOMP_PRESET=retro|crisp`): `--retro` keeps the PSP's low-poly look
(nearest-neighbour textures `PSPRECOMP_TEX_FILTER=nearest`, render scale 2, whole-number window scaling
`PSPRECOMP_INTEGER_SCALE=1` = 960x544 centred on a 1280x800 Deck panel, 480:272 kept, texture packs/upscaler off);
`--crisp` is render scale 3 with a bilinear aspect fit. The Deck's `play.sh` uses `--retro --fullscreen`. Explicit
flags beat presets. Details and the table in `docs/steamdeck.md`.

Window and scaling (generic runtime options; `run.sh` flags in brackets): the window is resizable and
defaults to the largest integer multiple of 480x272 that fits the desktop (1280x800 Deck: 2x = 960x544;
a 1440x900 Mac: 3x). `PSPRECOMP_SCALE=<n>` [`--scale n`], `PSPRECOMP_WINDOW=<w>x<h>` [`--window WxH`],
`PSPRECOMP_FULLSCREEN=1` [`--fullscreen`, borderless desktop fullscreen]; F11 or Alt+Enter toggles
fullscreen at runtime. The picture keeps its 480:272 aspect with black bars; `PSPRECOMP_STRETCH=1`
[`--stretch`] fills the window instead. `PSPRECOMP_FILTER=nearest|linear|integer` [`--filter`]: `integer` =
largest whole multiple, centred (crisp); `linear` = bilinear to the aspect-fit rectangle; the default is
nearest when the fit is an exact multiple and linear otherwise. `PSPRECOMP_INTEGER_SCALE=1` [`--integer-scale`]
uses the `integer` placement but blits nearest only when the render target is a whole multiple of the output (or
the reverse) and bilinear otherwise. `PSPRECOMP_TEX_FILTER=nearest|linear` [`--tex-filter`] forces the sampling
filter of game textures (default: what the game requests; render-target reads are untouched). High-DPI (Retina) uses the drawable size.
Debug-socket `S path.tga` still captures the native 480x272 frame; `V path.tga` captures the whole window.

Internal render resolution (generic runtime option): `PSPRECOMP_RENDER_SCALE=<n|auto>` [`--render-scale n`], 1..8 or `auto` (drawable height / 272, capped at 3),
default 1 (the Deck's `play.sh`: 2 with `--retro`, 3 with `--crisp`). Every GE render target (colour + depth) is allocated at 480n x 272n and the GL viewport is
multiplied by n, so 3D geometry is rasterised at n times the PSP resolution while 2D/HUD vertices (which are
PSP-pixel coordinates mapped to NDC) land in the same place. Game textures are not upscaled and keep the
filter the game sets. Framebuffer-as-texture effects (the battle's render-to-texture chain, blur) keep
working because their UVs are normalised; the window blit uses bilinear unless the window is exactly
480n x 272n (or `PSPRECOMP_FILTER=nearest|integer`). `S` box-averages the n x n blocks back to 480x272
(`PSPRECOMP_SHOT_FULL=1` writes the full-size frame instead). Block transfers only move guest memory and are
not scaled. Wide lines/points are requested at n pixels (best effort; core profiles may clamp). Scale 1 is
bit-identical to the previous output. Cost is roughly proportional to n^2 in fill rate.

Only the buffers that are shown on screen are rendered at n (a buffer becomes one when the game first displays it, or is the
first it draws into, and is then grown without losing its picture). Every other target the game draws into stays at 480x272, as in
PPSSPP (https://www.ppsspp.org/docs/troubleshooting/the-bloom-problem): the game's glow copies the picture, blurs it with a fixed
number of steps tuned for 480x272, and at n times the resolution those steps blur too little and leave a hard ghost around
characters and bright objects. `PSPRECOMP_RENDER_SCALE_ALL=1` scales every target again (the old behaviour).

Widescreen (generic runtime option): settings menu > Display > Ultrawide (saved as `widescreen=1`), or `PSPRECOMP_WIDESCREEN=1`.
Soft Focus (generic runtime option): settings menu > System > Soft Focus (saved as `soft_focus=0`), or `PSPRECOMP_SOFT_FOCUS=0`. Some stages (Order's Sanctuary) shrink the picture to a 120x68 thumbnail and blend it back over the whole screen for a soft, dreamy look; Off drops that pass for a sharper picture. On by default (the game's own look).
The PSP's 480x272 is already about 16:9, so this matters for windows and screens wider than that (21:9, 32:9). The displayed
buffers become as wide as the window's shape needs (`272 x aspect` PSP pixels, even, 480..960; never narrower than 480).
3D draws whose viewport spans the whole screen get their clip x scaled by `480 / width`, so the vertical field of view stays and the
horizontal one grows: the scene shows more to the sides (the battle camera is given the wide aspect, `camera.cpp`, so the game's own culling of objects and shadow casters matches the wider picture and nothing pops in at the edges; `PSPRECOMP_WIDE_CULL=0` goes back to the PSP's aspect for A/B tests). Every through-mode (2D) draw is scaled the same way, so the HUD and menus stay centred at their original size.
A through-mode draw that covers the whole 480x272 screen without a render-target texture (a menu background, a fade) is stretched
over the whole width instead, so menus show their background on the side parts. One that does read a render target (the glow
chain's add-back) covers the full width too but keeps its scale (its u is scaled about its centre; the texture's clamped edge fills
the sides, so no hard edge shows where the middle ends); other draws that read a render target stay centred; offscreen buffers stay 480x272, so the glow does not reach the side parts. The savedata dialog and the
settings menu are drawn centred. A buffer that was 480 wide when widescreen was switched on is grown without losing its picture.

Input: keyboard and SDL game controllers (`PSPRECOMP_SWAP_AB`, `PSPRECOMP_GAMECONTROLLERDB`, `PSPRECOMP_PAD_LOG=1`; key/button
tables and Steam Input setup in `docs/steamdeck.md`).

Controls, right stick (the PSP has none): in battle it is a free camera. `games/dissidia/runtime/camera.cpp`
wraps the battle camera update (`ovl_battle FUN_08B3CA24`, once per frame) and rotates the final eye position
around the fighter you control (the fighter nearest to the game's own eye; the whole rig, eye and look-at, turns about him, so he keeps his place on the screen; `PSPRECOMP_CAM_PIVOT=0` orbits the game's look-at point instead, the old way) by an accumulated yaw/pitch offset, so the game's own d-pad camera, lock-on framing,
auto-centring and collision pull-in keep working underneath (the stick is an offset on top: when you let go of the stick it
eases back to the game's camera after 0.5 s, with a time constant of 1 s; `PSPRECOMP_CAM_RECENTER=<s>` changes the time constant,
`PSPRECOMP_CAM_RECENTER_DELAY=<s>` the pause, `PSPRECOMP_CAM_RECENTER=0` holds the offset until you move it again); R3 (right stick click) or the C key puts the camera back at once. Stick right turns the view right (`PSPRECOMP_CAM_INVERT_X=1` for the other way). While the view is turned, the left stick is turned by the same angle (`PSPRECOMP_CAM_MOVE_REL=0` turns that off), so "up" goes into the screen instead of towards the opponent. When the fighter has no lock-on target (after having had one in the fight, or 10 s into it: `PSPRECOMP_CAM_HOLD_ARM=<ticks>`) the camera keeps following the fighter but holds its direction and distance: the left stick never turns it and it does not re-centre, only the right stick turns it (`PSPRECOMP_CAM_UNLOCK_HOLD=0` turns this off; `PSPRECOMP_CAM_FORCE_UNLOCK=<s>` is a test aid that reports 'not locked' that many seconds after the first lock; `PSPRECOMP_CAMLOCK_LOG=<n>` logs lock changes). The turned view is smoothed: the stick drives a turn speed that eases in and out (`PSPRECOMP_CAM_STICK_TC`, s), the view orbits your own fighter (not whichever fighter is nearest the lens) through a pivot that follows him with a short lag (`PSPRECOMP_CAM_PIVOT_TC`), its distance eases after the game's (`PSPRECOMP_CAM_DIST_TC`; the game zooms and pulls in for its own view direction), the held view follows jumps with a lag (`PSPRECOMP_CAM_HOLD_TC`), and re-centring ramps in. Your own view (turned or held) collides with the stage: the runtime keeps the main view's static stage geometry in world space and traces the line from the fighter's chest to the eye (`ge_draw_camera_probe_*`, a frame late); the eye pulls in fast when it is blocked and lets out slowly, so a turned view stays inside a room instead of looking at the back of a wall (`PSPRECOMP_CAM_COLLIDE=0` turns it off). What still blocks the view of your fighter fades to see-through (`PSPRECOMP_CAM_BLOCKER_FADE=0` turns it off, `PSPRECOMP_CAM_BLOCKER_ALPHA` sets how opaque, default 0.25): rays from the camera to the fighter's knees, chest, head and shoulders; each drawn instance of a non-skinned stage draw (up to 1500 vertices, not lower than his knees) they pass through is drawn blended with constant factors and no depth writes (`ge_draw_set_view_blocker`); it fades out in 0.08 s and back in over 0.3 s, also after the view returns to the game's. `PSPRECOMP_CAMF_LOG=<n>` logs the final eye of n camera updates (tests; the test script's `rstick <x> <y>` holds the right stick). Faded walls still count for the ray-traced shadows (the RT scene is captured separately from drawing). Outside battle (menus) the right stick acts as the
D-pad. Options: `PSPRECOMP_CAM_FREE=0` (stick is a plain D-pad everywhere, the game's own camera), `PSPRECOMP_CAM_SENS`
(default 1.0 = ~150 deg/s at full deflection), `PSPRECOMP_CAM_INVERT_Y=1`, `PSPRECOMP_CAM_PITCH_MIN/MAX` (eye elevation
limits in degrees, default -5..80), `PSPRECOMP_RSTICK_DEADZONE` (default 0.20), `PSPRECOMP_RSTICK_DPAD=0` (right stick
never becomes a D-pad). Headless test: debug socket `A2 <x> <y> <ms>` holds the right stick (0..255, 128 centre).
`PSPRECOMP_CAMX_LOG=<n>` logs n camera updates (game yaw, stick offset, eye, target).

Analogue stick direction: the game's own stick processing (`FUN_08872138`, pad object `+0x258` = 100.0) applies a
per-axis dead zone of 100/127.5 and then normalises, so a stick at 20 degrees reads as straight up and 60 degrees
as straight right: 8 directions on a PSP nub, only 4 on a circular-gate stick. `games/dissidia/runtime/input.cpp`
wraps it and replaces the stick vector (unit direction + magnitude, same layout) with a radial dead zone and the
true angle. The virtual D-pad bits the original returns (what battle movement reads) need a square gate and 78% on both
axes, so in battle the original runs on a square-gate version of the stick with its per-axis dead zone lowered
(`PSPRECOMP_ANALOG_DPAD_ZONE`, default 0.35 of travel; `PSPRECOMP_ANALOG_8WAY=0` turns it off). `PSPRECOMP_ANALOG_FULL=0` restores the
original 8-way behaviour, `PSPRECOMP_ANALOG_DEADZONE` (default 0.20 of travel) sets the radial dead zone,
`PSPRECOMP_ANALOG_LOG=<n>` logs raw / game / replacement vectors.

Walking direction: the stick vector above is not what steers a walk. The walker's input (`ovl_battle FUN_08B38A0C`, on the world map
and for the player's fighter in battle) builds its step from the four walk bits only, so even with the changes above every walk went one
of eight ways (found by VCKing101, [related-projects.md](related-projects.md)). `input.cpp` turns that step to the stick's own direction
just before the game applies the camera to it, for the hardware pad's walker with the stick past the dead zone; when the walk starts, the
walk speed and everything after it stay the game's. On the world map and in battle; never while the pads are fed by the game's online
exchange or a replay. A saved battle replay stores just the 8-way bits, so a fight steered by the stick can play back a little
differently; `PSPRECOMP_ANALOG_WALK_BATTLE=0` keeps battle 8-way (and replays exact). `PSPRECOMP_ANALOG_WALK=0` turns it all off,
`PSPRECOMP_ANALOG_WALK_LOG=<n>` logs n replaced steps, `PSPRECOMP_ANALOG_WALK_OUTLOG=<n>` the walk's world-space output.
Measured 18-19 November 2026: on the prologue field (debug socket `A` pushes of 0.4 s, walker position at `+0x110`) 15, -15 and 30
degrees of stick turned the walk by 14.8, 15.0 and 30.2 degrees against a straight push, where the 8-way walk jumps by 20 to 45. In
battle (battle_test, Squall, stage 12, stick 0 then 15 degrees) the walk's world direction turned 15.6 degrees and the fighter moved
along it to within 1 to 3 degrees a frame (more while the body turns); with the 8-way walk it stayed on one direction for 0, 15 and -15.
Comparing world headings between separate battle runs does not work: the lock-on camera orbits the opponent, and the dummy stands
somewhere else each run.

Camera follow dead zone: the world map's free camera (`ovl_battle FUN_08B3F7F4`) turns toward the walker's facing by up to 0.05 rad a
frame with no dead zone. With the walk at the stick's own angle, a push a few degrees off straight up then turns the camera, the
camera-relative walk turns with it, and the camera spins (VCKing101's measurement; here with `PSPRECOMP_CAM_UNLOCK_HOLD=0` and no
zone, a 15 degree push turned the walk 186 degrees in 1.75 s). `input.cpp` moves the follow's atan2 result (`FUN_089A457C`, only the
call on the walker's facing) so the follow does not turn within `PSPRECOMP_ANALOG_FOLLOW_ZONE` degrees, ramps up to the cap over half the zone, and is
the game's beyond that; only while the walker's last step was the analogue one. Default 180: the camera does not follow a walk with
the stick at all, since the right stick turns it (asked for 19 November 2026: the left stick also swung the camera left and right);
22.5 is VCKing101's zone, 0 the game's follow. Walking with the D-pad keeps the game's follow. With it the same push walked straight. Normally `camera.cpp`'s hold camera is active on the world map too (no lock-on target):
it keeps the view and turns the stick by the game camera's turn, which already hides the spin; the zone covers the cases without it
(`PSPRECOMP_CAM_UNLOCK_HOLD=0`, `PSPRECOMP_CAM_FREE=0`). `PSPRECOMP_ANALOG_FOLLOW_LOG=<n>` logs reduced follow turns.

## Updates compile only what changed (object cache), and how many files compile at once

Every update runs the build step again, because the game is linked with the runtime of the new release. Until 26 Nov 2026 it
also compiled all of the game's generated C++ again (about 470 files), although that code is almost never different:

- **Android (the phone's own build):** the object files were named after a hash of the compiler flags and of zig's path, and both
  contain the release folder (`release-<version>/...`), which is new with every update. No old object ever matched.
- **PC:** the translation step deletes and rewrites every generated source, so the build tool sees all of them as new; the build
  folder is also started over when the toolchain's folder changes (it is inside the release). ccache was used only when the player
  happened to have it.

The generated code depends only on the game, `game.toml`, the overlay files, the analysis data and the recompiler. The mod code
(tag team, HUD, ...) is in the precompiled core. From v0.1.15 to v0.1.25 (11 releases) the first four did not change at all and
the recompiler changed on one day; six runs of the recompiler in a row give the same bytes (only `recomp_fingerprint.h`, which
holds the time of the translation, and two report files differ).

**The compiled code itself is not changed by any of this:** the same compiler, the same flags (`-O2`), and a reused object is the
file the compiler wrote. Checked: after an update from the cache, the game was built once more with the cache off, and all 468
objects were byte for byte the cached ones. (The linked binary as a whole differs between two builds anyway: `main.cpp` carries
the time of the translation.)

### The cache (`tools/objcache.py`)

An object is kept under a key made from what it was compiled from:

- the source's bytes;
- the generated headers it includes (its `#include "..."` lines, followed into those headers: `recomp.h` and the module's
  `funcs.h`). `recomp_fingerprint.h` is not among them: only the runtime's `main.cpp` includes it;
- the compiler (its `--version` text without the lines that name folders, its size; on the phone zig's version, the NDK version
  and the API level);
- the compiler flags without folder paths (a response file is read and treated like the command line).

So a new release folder or a new work folder does not change a key, and a change of the code, of a header, of a flag or of the
compiler does. Sources outside `output/` and `overlays/` (the runtime's `main.cpp`) are compiled by the old rule (when newer).

- **PC:** `psp_build.py` makes `objcache.py` the compiler launcher of the CMake build (`CMAKE_CXX_COMPILER_LAUNCHER`). The objects
  are in `<work>/objcache` (about 70 MB); the Android build made on a PC has its own, `<work>/android/objcache`.
  `<build>/objcache.manifest/` holds one small file per object of the build folder (which entry it uses); after a build every
  other entry is removed, so the cache does not grow. An error inside the cache never fails a compile: the compiler just runs.
- **Android:** `android_build.device_build` names the objects in `<work>/android/obj` by the same key and removes the objects
  the build did not use.
- **Why a file was compiled:** the parts of each source's last key are remembered (`<cache>/why/`), and the build says which part
  changed:
  `Game code: 468 files compiled (why: the compiler changed), 0 unchanged since the last build (reused)` (PC),
  `Compiling 140 of 469 files with zig; 329 are unchanged since the last build (why: the header funcs.h changed)` (phone).
  The reasons: `first build`, `its code changed`, `the header <name> changed`, `the compiler changed`,
  `the compiler flags changed`, `its object was not kept`, `the cache format changed`.
- zig's own cache folder on the phone (`<work>/android/zig-cache`) held a second copy of every object and was never emptied: it
  is now emptied before and after the phone's build (`PSPRECOMP_KEEP_ZIG_CACHE=1` keeps it). A compile that fails with zig's
  `CacheCheckFailed` or a full disk now says how much space is free, and the remaining files are not started.

### Compile jobs

Before, at most 3 files compiled at once on any machine. Now, unless `--jobs N` is given:

- one job per core may run, and a job is started only while the memory that is free right now covers it: free memory >= what a
  compile needs x (1 + jobs started in the last 20 s) + 800 MB. One job always runs;
- what a compile needs is measured during the build (the largest peak so far; 1.5 GB until the first compile has finished):
  Linux, Android and macOS from the finished process (`wait4`), Windows from a job object (the wrapper, zig and clang together).
  The phone's build compiles the largest files first, so the worst case is known early, and prints each file's time and memory:
  `[<n>/<of>] batch_0102.cpp (<seconds> s, <peak> MB, <jobs> at once)`, and at the end
  `Compiled <n> files in <s> s: up to <jobs> at once (<cores> cores), largest job <peak> MB`;
- on a PC the launcher is the gate (the build tool starts one compile per core, each waits in the launcher); on the phone it is
  the build loop. Where the free memory cannot be read (macOS) the count is fixed: at most 4.

### Measured

A PC, 16 cores, 7 GB free for the build, g++; a source tree, so the runtime core was compiled as well (a release ships it
precompiled):

| build | before | now |
| --- | --- | --- |
| first build (everything compiled) | 427 s (4 jobs) | 303 to 319 s (memory gate) |
| update: new translation, build folder gone | all 468 game files compiled | 0 compiled, 468 reused |
| the reused objects against a fresh compile (cache off) | - | 468 identical, 0 different |
| update where one input had changed | all 468 compiled | 1 compiled (the file whose code changed), 467 reused |
| the same build again, nothing changed | - | nothing compiled, the cache stays (468 entries) |
| one generated source edited | - | 1 compiled (`why: its code changed`), its old object removed |

The phone's loop was run with a stand-in compiler (first build, nothing changed, one source / one header / the compiler changed,
the cache off and on again, a failing compiler). `test_objcache.py` passes on Linux and on Windows (Python 3.10).
**Not measured:** a phone, zig, a Windows build. The phone's log lines above give the real numbers.

### Switches, test, releases

- `PSPRECOMP_NO_OBJCACHE=1`: no cache and no gate (objects by time, a fixed job count). `PSPRECOMP_JOBS_FIXED=1`: a fixed job
  count, the cache stays. `--jobs N`: exactly N jobs. `PSPRECOMP_OBJCACHE_LOG=1`: one line per hit / miss (PC).
- Test: `python3 -I tools/test_objcache.py` (no game data, no compiler; Linux, macOS, Windows).
- **For a release: nothing to do.** A release that changes the recompiler, `game.toml`, an overlay file or the analysis data makes
  players compile the files that change (all of them when `recomp.h` changes), and their log says why; any other release
  compiles `main.cpp` and links.
- **Rules that keep it working:** no time and no path in generated sources; nothing generated includes `recomp_fingerprint.h`; a
  compiler flag that names a folder must be one the key leaves out (`normalize_flags`) or every update misses; raise
  `objcache.VERSION` when the meaning of a key changes.

### Not done

- The first install is still a full compile. A lower optimisation level would shorten it but makes the game's code slower, so
  it is not used (in-game smoothness comes first).
- The game code as its own shared library, so that an update would not link either.
- zig's global cache on a PC (`~/.cache/zig`, `%LOCALAPPDATA%\zig`) also keeps a copy of each object it compiles; it holds zig's
  own runtime libraries too, so it is not emptied. With the object cache it grows only when game code is really compiled.

## The phone's build profile (Fast / Full) and resuming

The setup app on Android compiles the game on the phone (`tools/android_build.py` `device_build`). The user picks, before the build, **Fast** (default) or
**Full** (stored in `files/build_profile.txt`, copied to `<work>/build_profile.txt` for the build; `Rebuild game (Full/Fast)` on the home page switches later).
PC builds and the SDK's APK build are unchanged (-O2 everywhere).

- **Full**: every file -O2 (as before).
- **Fast**: -O2 only for the generated batch files that contain a function listed in `tools/hot_funcs.txt`; every other batch is compiled with
  `-Os -fno-inline` (`COLD_OPT`; `PSPRECOMP_COLD_OPT` overrides, `PSPRECOMP_BUILD_PROFILE=full|fast` forces a profile). The flags are part of the object cache key
  (PR #140), so switching profile compiles only the files whose flags differ. `build: Fast: -O2 for N batch files ...` is in the log.
- `hot_funcs.txt` ("group addr", group = out / battle / menu / extra / supacc / xmods) is the set of functions that make up 97 % of the executed code
  (entry count x function length in generated lines) in a tag battle + the menus. It is matched by function entry address, not by batch number, so a renumbering
  or a changed batch layout does not matter; a function that is not in the list is simply cold. Regenerate: run a tag battle with
  `PSPRECOMP_PC_TRACE=1 PSPRECOMP_TAGTEAM=1 PSPRECOMP_TAGTEAM_EXBFC=2:75:<path>` (writes `<path>.1`, `<path>.2` entry counts) and
  `python3 tools/hot_funcs.py <work> <path>.1 <path>.2 tools/hot_funcs.txt`.
- Measured (host, zig 0.15.2 for aarch64-linux-android, the same flags, 468 files, 6 jobs, host under load so CPU seconds are noisy): -O1 costs the same as -O2 on this
  code (huge straight-line functions: the time is the inliner and the back end), -O0 is slower (41 MB objects), `-Os -fno-inline` is the cheapest.
  All -O2: 1185 to 1353 CPU s (wall 199 to 236 s); Fast: 825 CPU s (wall 139 s), 189 of 451 batch files -O2.

### Resuming a build that was cut off
Every finished object is put in place with `os.replace` of a complete `.part` file, so an object is whole or absent; `.part` leftovers of a killed compile are
deleted when the next build starts. psp_build keeps its translation stamp. The app records each finished plan step in `<work>/steps_done.txt` and the plan's
options in `files/build.running`; after a kill / crash / quit the app offers **Resume the build** (skips the finished steps, the compile step compiles only the
missing objects, then links and packs) or **Start over**. The desktop wizard shares the object cache and psp_build's stamps, so re-running its build also compiles only the
missing files; it has no per-step record or dialog (not needed: the steps are idempotent).

Game speed of Fast vs Full (PC x86 build of the same sources compiled the same way: batches in `hot_funcs.txt` -O2, the others `-Os -fno-inline`; the Android emulator rig was
in use by another session): a 70 s tag battle run unthrottled took 56 / 55 s (Full) and 53 / 59 s (Fast) wall, no measurable difference. SDK APK build (cmake, all files):
Full 2 m 59 s wall / 28 m 11 s CPU, Fast 2 m 16 s / 19 m 45 s. Not measured on a real phone.
