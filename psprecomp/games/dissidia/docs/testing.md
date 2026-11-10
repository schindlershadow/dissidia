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

## Known limits (2026-11-10)

- The stage is random and the CPU is a frozen dummy; fighters still get hit reactions (script 800) in some runs, which cuts a move short.
- Body position (+0x110/+0x118) is what a rush changes; the actor origin (+0x80/+0x88) does not move during moves.
- First results: the first script of most actions lasts the same at 30 and 120 fps; the rest of each window is noisy.
