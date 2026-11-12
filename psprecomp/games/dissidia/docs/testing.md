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
- **`PSPRECOMP_TEST_NO_AI=1`** (set by the harnesses): CPU brains (brain +0x54 mode 0) never run their tick (roster.cpp), so the CPU fighter does not act once the test clock runs (earlier the intro needs the brains).
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

### Findings so far (assist gauge, 2026-11-11)

- Side order in the gauge tables is NOT fighter order: Squall (the harness player) was side 1 and gained gauge from hits (+30 per
  hit step, +46 per update in some bursts); side 0 (Lightning, the CPU) starts at 2000 (20 %), side 1 at 0.
- Idle runs (gauge set to 8000, no input, or one tap then idle; CPU frozen or active; 30 / 60 / 120 fps): **no decay at all** for
  25-30 s. The per-update add FUN_08AEC1EC is called every update with amount 0 while idle. So a drain that is dt-wrong is not
  visible in these setups; the HUD's other moving bars (static word 0x08B937FC, red pixels in a screenshot) are NOT the assist
  gauge: judge only by `[GAUGE_HUD]` / `[ASSIST]`, never by pixels.
- Gauge changes come from event handlers (dispatcher FUN_08AEC558, jump table at 0x08AEC558-relative 0x08AF0000-13384, events 2..22);
  gains are per hit / action from tables, not per update.

## Making automated runs faster (notes for other sessions)

- Build only `cmake --build <dir> --target psprecomp_runtime` (the `test_debug_socket` target fails to link and aborts `all`).
  A fresh build dir recompiles ~100 generated files (~8 min at -j8); keep one per worktree and rebuild incrementally
  (a framerate.cpp edit relinks in well under a minute). Use `nice` so other sessions' runs are not disturbed.
- The menu walk dominates a run (3 min when other tests share the GPU/CPU; the fight itself is 10-40 s). Run the 30 and 120 fps
  runs in parallel (each `battle_test.py run` takes its own Xvfb display and debug port) instead of one after the other.
- Do not take a screenshot loop (`S` command) while measuring timing: it slows the game; and screenshots do not identify the
  gauge. Use the runtime logs.
- Set state directly instead of playing for it: script command `gauge <side> <value>`; `scan` + `RAW` for finding words.
- The CPU dummy (time step 0) is the default; `BT_NO_FREEZE=1` lets the CPU fight. Rounds can end early ("[TEST] battle ended") and
  restart: the game clock and the script restart with the next battle.
- A run sometimes fails with "the battle never started" or "Connection refused" when the machine is busy: just rerun it.
- Never `pkill -f` patterns; use exact pids; check `ps` for another session's psprecomp_runtime before touching anything.

## Assist gauge idle drain (fixed 11 Nov, branch assist-gauge)

- Gauge struct (`[ctrl+0x52C]`): `+0x18` the gauge, `+0x1C` the bar that starts at the battle's initial gauge (2000 for side 0) and
  runs down after a few seconds with nothing done, with an accelerating curve; `[GSTRUCT]` logs the struct (PSPRECOMP_ASSIST_GAUGE=1).
- Bug: the state machine FUN_08AED330 (-> 08AED83C -> 08AEE124) has drain terms per update instead of per step, so at 120 fps the bar
  emptied 3.7x faster (1500 -> 0 in 758 ms against 2767 ms at 30 fps; 60 fps 2x). Fix in `runtime/framerate.cpp` (`gauge_sm_hook`): above 30 fps
  the machine runs every rate/30-th update with the accumulated step (PSP 30 Hz behaviour); `PSPRECOMP_GAUGE30=0` turns it off.
- Test: `battle_test.py gauge-drain --work W` runs 30 / 60 / 120 in parallel with no input and checks the 1500 -> 0 time is equal
  (PASS: 2767 / 2767 / 2766 ms; with PSPRECOMP_GAUGE30=0 at 120: 758 ms). The time before the drain starts differs between runs because
  game time 0 is when the harness sees the HUD (a few hundred ms of jitter); compare durations, not start times.
- Gains from attacks still work at 120 with the fix (checked: gauge 2000 -> 2930 over the attack script).

## On an Android device (`tools/android_battle.py`)

Same menu walk, run on a phone over adb (the runtime's debug socket through `adb forward`, the game's `last_run.log`, screenshots pulled back):

    android_battle.py --serial <ip:port> --renderer vulkan [--driver ID] [--rt 1] [--fps 30] [--shots 4,8] [--env PSPRECOMP_SSAO=0] --out DIR

The game's `env.txt` is replaced for the run and put back afterwards; a private settings file is used. Saves are the device's own (Quick Battle must be
unlocked; autosave is off). The runtime's test driver only runs with a script, so an idle one is pushed when `--script` is not given.

