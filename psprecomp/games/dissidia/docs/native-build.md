# The native build in detail

How the three setup steps work, the work folder, and every runtime option (window, scaling, render resolution, input, camera). The short install steps are in the [top-level README](../../../../README.md#install-and-play); the Steam Deck walkthrough is [steamdeck.md](steamdeck.md). Paths are relative to `psprecomp/games/dissidia/`.

## What each step does

You need your own copy of the game, a 64-bit Linux PC or a Steam Deck, about 8 GB of free disk space, and patience:
the first build takes a long time (tens of minutes to hours). The steps, for the Deck and for a Linux PC, are in the
top-level README: [Install and play](../../../../README.md#install-and-play). Steam Deck details (Steam, controls,
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
   `<workdir>/build/psprecomp_runtime`. Re-running it is safe. Add `--jobs 2` if it runs out of memory.
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

Widescreen (generic runtime option): settings menu > System > Widescreen (saved as `widescreen=1`), or `PSPRECOMP_WIDESCREEN=1`.
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
`PSPRECOMP_CAM_RECENTER_DELAY=<s>` the pause, `PSPRECOMP_CAM_RECENTER=0` holds the offset until you move it again); R3 (right stick click) or the C key puts the camera back at once. Stick right turns the view right (`PSPRECOMP_CAM_INVERT_X=1` for the other way). While the view is turned, the left stick is turned by the same angle (`PSPRECOMP_CAM_MOVE_REL=0` turns that off), so "up" goes into the screen instead of towards the opponent. When the fighter has no lock-on target (after having had one in the fight) the camera is held where it was: the left stick does not move it and it does not re-centre, only the right stick orbits it (`PSPRECOMP_CAM_UNLOCK_HOLD=0` turns this off; `PSPRECOMP_CAM_FORCE_UNLOCK=<s>` is a test aid that reports 'not locked' that many seconds after the first lock; `PSPRECOMP_CAMLOCK_LOG=<n>` logs lock changes). Outside battle (menus) the right stick acts as the
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
