# Automated battle tests (no menus, no display)

`tools/battle_test.py` boots the game headless (its own Xvfb display, real GPU if the box has one), walks the menus to a
Quick Battle (Squall vs a CPU fighter), plays a scripted pad input or starts moves directly, and quits. A private copy of the
work folder's memory stick and a private settings file are used, so the user's saves and settings are never touched.
Needs: Xvfb, python3 + Pillow, a save in `<work>/ms0` (Quick Battle unlocked) and a runtime built with the test driver.

    battle_test.py run     --work W --script S --fps 120 --out DIR     # one run
    battle_test.py compare --work W --script S --rates 30,120          # same script at each rate, paths lined up by game time
    battle_test.py actions --work W --actions 2423,2424 --rates 30,120 [--hold SQUARE --stick 128,255 --hold-ms 1500]
    battle_test.py actions --work W --all                               # every action the fighter has

## Pieces

- **Script player** (`runtime/framerate.cpp`, `PSPRECOMP_TEST_SCRIPT=<file>`): lines `<game ms> set|tap|analog|mark|action|quit`.
  Game time starts when the fight's HUD is up (`PSPRECOMP_TEST_GO=<file>`, created by the harness) and advances by dt, so the
  same script means the same at every frame rate. The scripted pad replaces keyboard / pad / debug-socket input.
- **`action <id> [who]`**: starts any action of a fighter without a press (same path as the roster touchdown / chain-window
  starts: the actor's own plya record + `FUN_08B382B8`). `action -1 <slot>` lists the fighter's actions (`[TEST] plya action <id> script <script>`).
  `who`: `player` / `cpu` or a slot number (0 = the first fighter seen = the player, 1 = the CPU).
  Moves that end when a button is released (Revolver Drive: Square held, stick back) need the button held in the script too.
- **`PSPRECOMP_INPUT_RECORD=<file>`**: writes the pad state the game was handed as a script (play a move by hand once, replay at any rate).
- **`PSPRECOMP_TEST_FREEZE_CPU=1`** (set by the harness): every fighter but the first gets time step 0 while a test runs (a dummy).
- **Logs**: `[FIGHTER] gt=<game ms> ... cur=<script now> pos=<x,z of the body root, +0x110/+0x118>` for every fighter every update in test runs;
  `[MOTION]` lines for spawned objects carry `gt=` and position.

## Setup the harness fixes

Stage 12 (`--stage N`, 1 = Random, default 12: the STAGE row of the battle setup page), Squall vs Lightning. The CPU is a dummy
(time step 0, HP kept at its starting value, actor +0x10B0, so a hit never ends the fight and resets the clock).
Revolver Drive is a ground HP attack (hold the HP attack button, Triangle by default): actions 2289 (script 10004, the start) and
2290 (script 10005, the drive); `--hold TRIANGLE --stick x,y`.

## Known limits (2026-11-10)

- The stage is random and the CPU is a frozen dummy; fighters still get hit reactions (script 800) in some runs, which cuts a move short.
- Body position (+0x110/+0x118) is what a rush changes; the actor origin (+0x80/+0x88) does not move during moves.
- Results are taken from the move's first script only (what follows is recovery / walking).
- First results (Revolver Drive 2290, stage 12, Triangle held, stick forward / left / right / back): 1.2 s, 2.5-3.4 m, peak ~10 m/s at both
  30 and 120 fps; the animation clock (+0x1A8) advances 60 units per game second at both. So the drive's logic position / timing and animation
  speed are NOT faster at 120; suspects left: the camera, effects, and turning (heading change per update).

## Assist gauge tools (2026-11-11, branch assist-gauge)

- `battle_test.py scan --work W --script S --rates 30,120 --at 3000,13000 [--range 08800000-0A000000]`: RAM snapshots at game times
  at each rate (debug socket `RAW`), lists float words whose change per game second differs between the rates. Noisy (heap
  addresses move between runs); good for narrowing a value down, then use a watch.
- Script command `gauge <side> <0..10000>`: sets a side's assist gauge (side 0 / 1 = HUD controller order, not fighter order).
- `PSPRECOMP_ASSIST_GAUGE=1`: `[ASSIST]` (gauge float changes), `[GAUGE_HUD]` (what the HUD reads, percent), `[GAUGE_ADD]`
  (every call of the add routine with the amount), `[GAUGE_DRAIN]` (changes made by the event handler FUN_08AEC7D8).
  `=2` arms the memory-write watch on the first gauge change after 0.5 s.
- `PSPRECOMP_FIGHTER_DUMP_RANGE=<lo>-<hi>` (hex actor offsets, below 0x1400) limits the field dump to one range.
- Layout: gauge float at `[ctrl+0x52C]+0x18`, ctrl = `[[0x089D6A10]+56+4*side]` (ovl FUN_089E95B0); 0..10000; the HUD shows
  value x 100 / 10000 (FUN_089E97FC). Writers: FUN_08AEC1EC (add, clamped), FUN_08AEC628 / 08AEC6FC (int add), FUN_08AEC000 (per
  update tick: passive gain via FUN_08AEC338 and a drain that multiplies by the global step 0x089D5EAC), FUN_08AEC7D8 / 08AECA90
  (event handlers that subtract).
