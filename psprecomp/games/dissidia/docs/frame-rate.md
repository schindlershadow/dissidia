# 60 fps and the settings menu

## Using it

- **Settings menu**: Esc or F1 on a keyboard, Guide or L3 + R3 (both stick buttons) on a pad. The game pauses while it
  is open. Rows: Display (Fullscreen / Window), Window Size (a whole multiple of 480x272, or 1280 x 800, the Deck's
  screen, when the desktop is big enough; in fullscreen it is remembered for Window mode), Render Scale (1x to 4x),
  Frame Rate (30 / 60), FPS Counter (a small transparent frames-per-second number in the top-right corner; saved as
  `fps_counter`, `PSPRECOMP_FPS_COUNTER=1` turns it on without the menu), Quit Game (press twice). Left / right changes a value, Cross cycles it, Circle or Start closes.
  It is drawn from the game's own menu textures, like the save dialog.
- **Debug page**: L / R (shoulder buttons), or Q / E / Tab on a keyboard, switches between System and Debug:
  - Performance Overlay: for debugging. Frames per second, average and worst frame time over the last half second,
    presents per second (a gap to the fps means the presenter runs without new game frames), render scale and process
    memory (Linux), top-left.
    Saved (`perf_overlay`); `PSPRECOMP_PERF_HUD=1` turns it on without the menu.
  - FX Shaders: bypasses the `mods/fx_shaders` overrides live ("Not loaded" when none were loaded at start).
    This session only.
  - GPU Sync: the `--ge-sync` modes live (Off, Every batch = 1, Every list = 2, Every frame = 3, Flush only = flush).
    This session only.
  - Vertex Upload: Map / Sub (`PSPRECOMP_GE_RING`). Saved (`ge_ring`) and applied at the next start.
  The last three exist for the Steam Deck flicker hunt: flip them mid-battle instead of relaunching per test.
- **On-screen keyboard** (naming the player at the first start): a key grid in the save dialog's style. Pad: d-pad
  moves, Cross types the key, Circle deletes, Triangle = space, Square = upper / lower case, Start = done. A real
  keyboard types directly (Enter = done, Backspace deletes). On a Steam Deck, Steam's own keyboard opens with it
  (its Enter key finishes the name); `PSPRECOMP_STEAM_KEYBOARD=0` keeps it closed, `=1` forces it elsewhere. Look
  for `[OSK]` lines in the log if it doesn't appear. `PSPRECOMP_OSK_TEXT=<name>` or `PSPRECOMP_OSK_AUTO=1`
  skips it (headless runs), as before.
- **Load trace**: the last 32 PACKAGE.BIN reads (time, offset, size, index crc, and the mod file when one was
  served) are printed as `[LOAD]` lines when the game quits, so a load that never finishes can be traced by quitting
  from the menu. A mod file shorter than what the game asked for is logged as it happens. `PSPRECOMP_LOAD_TRACE=0`
  turns it off.
- Changes apply at once and are saved to `<workdir>/settings.ini`. On the next start the saved values win over
  `run.sh` / `play.sh` flags. Delete a line (or the file) to go back to the flags.
- Without the menu: `run.sh <workdir> --fps 60` (`PSPRECOMP_FPS=60`).
- `PSPRECOMP_FPS_LOG=1` logs the logic rate, dt, the global step and how often
  body poses change from one frame to the next (about 100% = animation runs at 60, about 50% = it steps at 30)
  every 5 s, and each 30/60 switch.
  `PSPRECOMP_FPS_SCOPE=all` also runs menus at 60 (they then run fast; for testing only).
  `PSPRECOMP_SETTINGS_MENU=0` disables the menu, `PSPRECOMP_SETTINGS=<file>` moves the settings file.

## In multiplayer

In an ad-hoc session ([multiplayer.md](multiplayer.md)) the game runs at the lowest rate any player asked for, so a
60 fps player and a 30 fps player both run at 30; the setting returns to normal when the session ends. Every player
shares the wanted rate in their beacons; `frame_end_hook` in `runtime/framerate.cpp` applies the cap. A toast (`ge_draw_toast`, top centre, 5 s) shows the agreed rate when it changes.

## Above 60 (90, 120 ... 240)

Any multiple of 30 up to 240 can be chosen (Frame Rate row, or `PSPRECOMP_FPS=120`). Above 60 the simulated vblank
clock itself runs at the chosen rate (`psp_display_set_vblank_hz`, the vblank count carries on without a jump), the
frame timer keeps interval 1 so each frame waits for one short vblank, and `frame_end_hook` writes
`dt = 60 / rate` ticks (0.667 at 90, 0.5 at 120) into the timer after the original frame end. Everything that was
scaled for 60 Hz uses the same factor (`interval / 2` became `ticks / 2`), and the effect components that step once per
update run one update in `rate / 30` with dt 2. **Not yet tested in play**: expect the unknown, such as integer
fades that truncate at a small dt. There is no vsync, so a rate above the monitor's refresh tears or drops frames.
In multiplayer the lowest rate any player asked for wins, whatever the values.

## What 60 fps means here

It is **native**: the battle simulation runs 60 updates a second. Nothing is interpolated, and there's no extra
display latency. It costs twice the CPU and GPU time of 30 Hz.

## How the game paces itself

The EBOOT is byte-identical between the US and EU discs, so these addresses hold for both.

| What | Where | Notes |
|---|---|---|
| Frame timer object | pointer at `0x089D69E0`, built by `FUN_08883E74` | `+0x04` mode (0), `+0x0C` vcount at the last flip, `+0x10` **interval in vblanks (2)**, `+0x14` vblanks this frame, `+0x18` **dt** (float), `+0x24`/`+0x25` dt enables |
| Frame end | `FUN_08883F9C(timer, useNonCB, flag)` | Every loop ends here (11 call sites: battle, menus, loading, movies). Waits until `interval` vblanks have passed since the last flip, swaps, stores `dt = vblanks x 1.0` (`0x089C9F50`). In mode 0 the count is clamped to the interval: **dt is a fixed 2.0 at 30 Hz**, in 1/60 s ticks |
| Interval readers / writers | constructor only; `FUN_08883F54` (inside the frame end) reads it | So the interval can change at any frame |
| Global step | `0x089D5EAC`, written by `FUN_08864DD8` | `= timer dt`, or a slow-motion value derived from it |
| Actor step | `FUN_08838300(actor)` | `actor clock (+0x3C8) x global step`, or the actor's own clock (`+0x3CC`) when it is flagged to ignore slow motion. 217 call sites |

The battle code is written against that step: 85 reads of `timer+0x18` and 146 of `0x089D5EAC` in the battle overlay
(31 and 48 in the EBOOT). Motion time, timeline events (keyed events fire in `(prev, now]`), movement (speed x dt),
decays (`pow(a, dt/2)`), effect lifetimes and the camera all advance by it. Comparisons against dt are countdowns
(`timer <= 0`) and float accumulators truncated to ints, which work at any step. So **interval 1 gives dt 1.0 and
the same game at 60 updates a second**. Menus use dt in 4 places only and count frames otherwise, so they stay at 30.

## What does not follow the step (fixed in `runtime/framerate.cpp`, only at 60 Hz)

| Code | Problem at dt 1 | Fix |
|---|---|---|
| Pad auto-repeat `FUN_08871BD0` | Repeat timer counts down by `max(step, 2.0)`: menu cursors would repeat twice as fast | Reload values (pad manager `*0x089D6008` `+0x25C` delay 30, `+0x260` interval 4) scaled by `2 / step` for the call |
| Actor own clock `actor+0x3CC` | Set to a constant 2.0 by the battle overlay (`0x08A374B0`, `0x08A37868`, `0x08A37960`, `0x08B46BE0`): those actors would run at double speed | `FUN_08838300`'s result scaled by `interval / 2` for such actors |
| Slow motion (`FUN_08864DD8`, object at `*0x089D5EA8`) | While it is on, the global step is the object's own value (`+0`: 0.4, 0.15 ...; a normal 30 Hz frame is 2.0), and the time left (`+4`) drops by the same value every frame. At dt 1 the world ran twice as fast as meant and the slow motion ended in half the time. An assist runs on a clock of 5.0 (5 x 0.4 = 2.0, the PSP's normal speed), so it ran at twice normal speed while the rest crawled | `+0` scaled by `ticks / 2` for the update, then put back |
| Root motion of looping motions (`FUN_0882F784`, motion player `+16..31`) | A looping motion (Kain's Sky Spear dive, Sky Rave) moves the fighter by the change of the root bone's pose each tick. On the last tick before the loop wraps (player time `+120` + dt reaches the end `+112`: time 31 of a loop ending at 32) the change is minus the whole loop's travel (5.3 against 0.48 on other ticks), so every 12 ticks the fighter was thrown back by what he had covered and the move hardly moved. At 30 Hz time steps by 2 and never lands on that tick | On that tick a change over 3x the previous tick's (and over 0.5) is dropped; one tick in 12 of travel is lost. `PSPRECOMP_FIGHTER_LOG=1` logs each drop as `[ROOTMOTION]` |
| Steered ground move controller `ovl_battle FUN_08A2C960` (Squall's Revolver Drive rush; timeline op 0x0225 *Steer on ground*, actor controller `+0x3A4`) | Writes the distance the actor moves **this update** (0.595 m at full speed) and never reads the frame time: 30 updates a second on the PSP, so 120 fps ran the rush 4x as fast (48 m instead of 21 m over a drive) | Its output vector scaled by `dt / 2` (`PSPRECOMP_STEER_MOVE=0` turns it off for A/B). Found with the test harness (docs/testing.md: replay of a recorded input at 30 and 120 fps, then a gdb write trace on the position: `PSPRECOMP_ALLOW_PTRACE=1` + an unstripped build). Checked: 6.0 m vs 6.1 m after 0.75 s, 20.4 m vs 20.8 m in total, same peak speed (14 m/s) at 30 and 120. Other moves that use this controller are covered; turning and acceleration inside it are still per update (not checked) |
| Assist gauge state machine `ovl FUN_08AED330` (-> `08AED83C` -> `08AEE124`, one call per update per side) | The idle drain of the gauge's second word (`[ctrl+0x52C]+0x1C`, the bar that starts at the battle's initial gauge and runs down while nothing is done) has terms per update, not per step: at 120 fps the bar emptied 3.7x faster (1500 -> 0 in 758 ms against 2767 ms at 30), at 60 2x | Run every rate/30-th update with the accumulated step (`gauge_sm_hook`, `PSPRECOMP_GAUGE30=0` for A/B). Checked with `battle_test.py gauge-drain`: 2767 ms at 30, 60 and 120 |
| UI animations with a fixed step (`FUN_088CF550` stores the UI step at `0x089D7834`: 0 paused, the frame dt when UI manager `*0x089D7830` `+33` is set, else a fixed 1.0; reader `FUN_088DA520`) | The fixed 1.0 per update ran twice as fast at 60: the "VS <name>" banner that opens a fight lasted 0.3 s instead of 0.6 s | Fixed step scaled by `ticks / 2` for the call (`PSPRECOMP_UI_STEP=0` turns it off). Measured with screenshots at 30 and 60: banner 0.62 s both |
| Camera mode 2 (`FUN_08B3B4FC`, camera `+0x100 == 2`) | Uses camera `+0x104`, a constant 2.0 from the constructor | Result scaled by `interval / 2` |
| Mod code (`roster.cpp`) | The warp-dash link window counted bone passes; `pinspin=` turned per pass | Counted in ticks (`dissidia_frame_ticks()`); spin and the own-clock copy scaled (`dissidia_own_clock_scale()`) |
| Effect components that step per update (see below) | Envelopes strobed, sparks / lines lived half as long and flew twice as fast, trails were half as long | Stepped at 30 Hz: every other update with dt x 2 |

Scope: the frame end hook writes interval 1 only while the battle overlay is resident (overlay id 1), 2 elsewhere.

### Effect components (fixed 1 Nov)

The effect system advances its components by `dt` = effect instance `+352` (the global step x the handle's scale,
written by `FUN_0894B48C`): 2.0 per update on the PSP, 1.0 here at 60 Hz. Most components scale by it, but these step
once per **update** (archive/open-questions/fx.md item 7, archive/fx-components/F.md, E.md):

| Channel | Update | Per update, not x dt | At 60 Hz before the fix |
|---|---|---|---|
| 61 envelope (on colour, via 62) | m2 `0x08953A50` | `mult += rate` (the timer does `+= 0.5 dt`) | each ramp twice as many steps of the same rate: 2x overshoot, clamped by 62 into a strobe |
| 95 points, 100 streaks, 102 line burst, 103 line rays | m2 `0x0897F3E4`, `0x0897DE38` (→ `FUN_08984508`), `0x08974DCC`, `0x089760E8` | life −1, motion, history (only emission is x dt) | half the life, twice the speed, same particle count |
| 68 trail path (for the ch80 ribbons) | m2 `0x08966018` (→ `FUN_0896B710`) | one ring-buffer sample | ribbons half as long |
| 70 smoke path (for ch82) | m2 `0x08967438` (→ `FUN_0896C9B0`) | shift + drift | twice as fast |
| 105 locus renderer | m2 `0x08964B40` | UV scroll `u -= P+48, v -= P+52` | texture scrolls twice as fast |
| 106 locus history (weapon / body trails) | `FUN_0896808C` (from m2 `0x089672C4`) | one shift whenever `t += dt` reaches `last + 1`: every update at dt 2 **and** at dt 1 | trails half as long |

Fix (`runtime/framerate.cpp`, only at 60 Hz, `PSPRECOMP_FX60=0` turns it off for A/B checks): each of these runs on
every other update of its own state block, with `instance+352` doubled for that call, so it does exactly what the
PSP does on a 30 Hz frame and holds in between (as the PSP's picture does). The emitters read dt through the current
runner `*0x09B6F478 +136`, the same instance. ch106 keeps a live trail head: the updates between run with dt 0 (no
shift, but `pos[0]` still follows the bone), and before each shift update `pos[0]` of both point buffers is put back
to the point of the previous shift update, so the shift copies the sample the PSP would. It is wrapped at
`FUN_0896808C`, inside `roster.cpp`'s lingering-trail hook on the m2. Checked with fakes of these functions driven at
30 Hz and 60 Hz: identical states on every 60 Hz update (held ones included) and the trail head on the bone every
update. Not yet checked in a game run. Checked and fine as they are (time- or dt-based, or event-driven): the
transform / colour curves (1–9, 15–17), 10, 18, 20, 21, 26, 80, 101, 107, 109–111, 150, 151; 66 (a rope with a
fixed link length) depends on distance, not on the update count; 79 is inert. Not read: 58 and 82 (one node each).

## First run (28 Oct, Mac M2 Pro, render scale 1)

Menus held 30 Hz (interval 2, overlay 2). The battle switched to 60 Hz as it loaded and ran at 59.4-59.8 logic updates
and presented frames a second for the whole fight (the first 5 s window, 50.7, includes the switch). Body poses changed
on 97-100% of frame-to-frame checks (one window 79%, likely hit-stop or a pause): the engine samples motions at
fractional frames, so **body animation runs at 60 with no extra work**. Clean exit, no new lookup misses.

## Still to check in play

Found statically and not yet confirmed in a run: anything in the battle code that counts frames as integers without
dt (none found among the dt readers; code that never reads dt can't be found this way), integer fades whose
`rate x dt` truncates to 0 at dt 1 (`FUN_08911AA8`-style `byte += trunc(rate x dt)`), the 30 fps GMO motions sampled
at half frames (confirmed fine in the first run), and the CPU cost on the Deck.

## Settings menu internals

Generic runtime code (`runtime/src/psp_settings.cpp`, see ARCHITECTURE.md). The page layout lives next to the save
dialog (`psp_sddialog::render_menu`) and uses the same `SdTheme`. The Dissidia theme adds the d-pad left/right glyphs
(button sheet, entry 8) and the blue/white selector triangles (common UI sheet, entry 0) for the value arrows.

### EX Burst quicktime and release gauge (fixed 10 Nov)

The EX Burst code steps with fixed amounts per call and counts in integers, not by dt. Above 30 Hz the quicktime after the HP
attack, the countdown and the release pointer ran 2x (60 Hz) to 4x (120 Hz) too fast. Two pieces:

| Code | What it does per call | Hook |
|---|---|---|
| EX Burst controller `0x08A3FE10` (battle overlay; one call per update from `FUN_088413D0`) | Phase state machine (`+632`: 1 start, 2, 3 slow motion and prompt, 4 release gauge). Phase 4 calls `0x08A3E4A8`: pointer position `+596` += velocity `+604`, velocity += constants, decay, no dt. `+636` is an int timer advanced by `trunc(timer + global step)` | `exb_ctrl_hook` |
| Quicktime UI `0x08A40E60` (calls the `0x08A41558` / `0x08A41774` states, which set game speed 0 with `FUN_08A62020` and wait on a timer) | Fixed steps; the countdown reads the frame timer's dt (`+0x18`), not the global step | `exb_ui_hook` |

Fix: each runs once per 30 Hz frame's worth of time (accumulator of `ticks / 2` per update, the other updates skip it) and for that
call the global step (`0x089D5EAC`) and the frame timer's dt are scaled by `2 / ticks`, so it sees what a 30 Hz frame has. Three things
had to be right, each found by the way it failed:

- **Sticky presses.** The code reads "pressed this update" with `FUN_08B326C4(actor)`, true for one update. On a skipped update the
  press was lost: the burst waited for a button press it never saw (a freeze before the minigame) and the timing felt off. On skipped
  updates the actors the burst asked about are polled and the edge is kept until the next run.
- **Timer dt as well as the global step.** Scaling only the global step left the countdown at a quarter of its speed (2:00 held for
  12 s). The PSP counts 2:00 to the end in about 4 s.
- **No double gating.** The UI is called from inside the controller; gating both throttled the inner one to every 16th update and the
  pointer jumped back and forth. Inside a gated call the other hook passes through.

The cost: the prompt and the pointer move 30 times a second while the picture draws at the full rate (looks slightly steppy).
`PSPRECOMP_EXB30=0` / `PSPRECOMP_EXB_UI=0` turn each hook off for A/B checks; `PSPRECOMP_EXB_LOG=1` logs `[EXB]` (phase, timers,
pointer position / velocity, step) and `[EXBP]` (button prompt `0x08A3D1C4`, which turned out not to be the quicktime). Checked by
play on Jecht at 120 Hz (video against a 30 Hz run). Cloud of Darkness uses the same code. Not checked at 60 / 240 Hz.

#### Kain / Terra mash prompt (fixed 12 Nov)

The EX Burst command prompt (bottom-middle bar with a button icon; the bar on the right is the foe's EX defence, driven by the CPU's pad)
reads the displayed button through a per-button test (`FUN_08B328CC` = Square, ... 37 tests `08B30B54..08B3315C`, virtual id in the
second operand) from the gated burst code. Only two of those tests had sticky edges, so at 120 fps about five presses in six were lost.
Every test the gated code calls now gets a per-(test, actor) sticky edge, polled on skipped updates (`vt_probe` / `vt_poll_held` in
`runtime/framerate.cpp`); the press latch is bypassed for these tests so a press counts once.

Debugging aids: `PSPRECOMP_INPUT_DEBUG=1` shows the pressed buttons on screen and logs `[INPUT]` lines; `PSPRECOMP_EXB_LOG=1` logs one
`[EXBG]` row per burst update (bar, speed, presses handed to the burst), every true button test with the burst time, and a per-phase
call / true summary of all 37 tests (`=2` adds the verbose `[EXB]` lines). Headless: `exstate 6` (test driver) starts a real burst;
`battle_test.py --char kain|terra`.

### Timed button inputs above 30 Hz (press latch)

The fighter code asks the virtual pad whether a button was *pressed this update* (`FUN_08AB399C`: state and not previous state), so
a press is seen for one update: 33 ms on the PSP, 8 ms at 120 Hz. Timed chain inputs (Jecht's 3 to 5 frame windows) were four times
harder to hit at 120 Hz. The hook in `runtime/framerate.cpp` reports a press as pressed for 33 ms in total (4 updates at 120 Hz),
ending early when the fighter starts another action (script id changes), so the press that starts a move cannot also trip that move's
early-press lockout (event 0x004C). `PSPRECOMP_PRESS_LATCH=<ms>` changes the time (0 turns it off); the `[FPS]` log line shows the counts.

### Jecht chain windows at 30 vs 120 Hz (measured 11 Nov, not fixed yet)

`tools/combo_timing.py combos` (docs/testing.md) presses the next chain input D ms after the previous move's script id first appears and records
which move follows. Every ground step of Jecht's wiki combos, 30 Hz against 120 Hz (D in ms, grid 8.33 ms; "start..end" = first and last D that start the chain move):

| Step | 30 Hz | 120 Hz before the fix | 120 Hz with the step (default now) |
|---|---|---|---|
| Rush > Neutral 2 (Circle) | 267..358 | 317..408 | 292..383 |
| Rush > Up 2 (Circle + Up) | 275..358 | 317..408 | 292..383 |
| Neutral 2 > Neutral 3 | 542..633 | 567..658 | 542..633 |
| Neutral 2 > Up 3 | 542..617 | 567..658 | 550..633 |
| Neutral 3 > HP (Square) | 600 (one sample) | 625 (one sample) | 600 (one sample) |

The windows are the same width (12 grid steps, 3 updates at 30 Hz, the wiki's "3f") but sit **25 ms (1.5 ticks) later at 120 Hz**, at both edges, for every step.
(Rush steps differ by 50 in D because a 30 Hz press is only seen on a 33 ms update boundary; in update time the shift is 25 ms.)
25 ms = 2.0 - 0.5 ticks: the first update of a move advances its clock by a whole 30 Hz frame (2 ticks) on the PSP and by only 0.5 at 120 Hz, so the move's
first visible frame is 33 ms into the move at 30 Hz and 8 ms at 120 Hz, while the windows are set in ticks of the move's own clock.
Fix (`actor_dt_hook`, on by default, `PSPRECOMP_ACTION_START_STEP=0` turns it off): above 30 Hz the update after a fighter's script id changes runs with a 30 Hz sized step (x `2 / ticks`),
so the move's clock is where the PSP's is at its first visible frame. Checked on Rush > Neutral 2 (D = press delay after the move's script id appeared): 30 Hz 267..358, 120 Hz 292..383
(before: 317..408). In terms of when a press reaches the game the 120 Hz window now opens about 17 ms earlier and closes about 17 ms later than the 30 Hz one (a 30 Hz press is only seen on
a 33 ms update boundary, a 120 Hz one on an 8 ms boundary, and the press latch holds it 33 ms), centred on the same moment instead of 25 ms late. All five were re-measured with the step (`combo_timing.py combos --rates 30,120`, 11-12 Nov): the Rush steps now straddle the 30 Hz window by about 17 ms each side, the Neutral 3 / Up 3 steps match to within one grid step (8 ms). The HP step still has one sample each (the sweep for it starts 300 ms after the previous move and the window is narrow); it needs its own finer run.
Not checked: 60 Hz, other characters (every fighter's first update after an action change is longer now; the dash / rush distance of the first update is 4x at 120 Hz, as it is one whole 30 Hz update at 30 Hz).
Exactly equal edges would need the in-action pressed test (`FUN_08AB399C`) to answer only on the move's 30 Hz lattice (every 4th update at 120 Hz); not implemented because it adds up to 25 ms input latency for every character's in-action presses.
Wiki windows for reference (30 fps frames, the page's Eddienput waits): Rush > N2/Up2 W10-12, N2 > N3/Up3 W18-20, N3 > HP W20-23 (4f), Up3 > UJS W27-31 (5f).
The HP step above has one sample because the sweep started its probe 300 ms after the previous move; the HP window needs its own finer run.

### Jecht timed windows on the 30 Hz grid (12 Nov)

The game compares a press with a window using the move's motion time (`actor +0x1A8`, ticks; the chain check `FUN_08AB81E0`, table element `{id, start, end}`, `start <= time < end`; the HP
finishers use the flag gates, see testing.md "Jecht Combo Timing"). On the PSP the pad is read once per 33 ms update and the clock is 2, 4, 6 ... at each read, so a press is
checked at only a few moments (3 for a 6-tick window). The first-update step above fixed the clock; what was left (windows up to 17 ms either side of the 30 Hz ones) was the 8 / 17 ms
read times. For the human Jecht the game is now handed Circle / Square on a 30 Hz grid above 30 Hz (`jecht_grid_filter`, via the new `psp_ctrl_set_filter` hook in psp_hle_ctrl.cpp):

- the buttons change only on every P-th update (P = 2 / ticks: 4 at 120 Hz, 2 at 60 Hz), counted from the first update after his last move change (`g_grid_anchor`), so the first read after a move
  starts comes right after its first visible frame and then every 33 ms; a tap that came and went between two reads is latched and seen once;
- together with the first-update step (the clock at read n is 2 + 2n at every rate) the windows are read at the same real times at 30, 60 and 120 Hz.
- Cost: Circle / Square reach the game up to one 30 Hz frame late (about 17 ms at 60 Hz, 25 ms at 120 Hz), only while the player is Jecht. The stick and the other buttons are live.
- `PSPRECOMP_JECHT_GRID=0` turns the input grid off (the first-update step stays); `PSPRECOMP_ACTION_START_STEP=0` turns that off.

Measured with `tools/combo_timing.py combos --rates 30,60,120` (accepted press delays after the move started, ms, grid 8.3 ms; 3517 s run):

| Step | 30 Hz | 60 Hz | 120 Hz |
|---|---|---|---|
| Rush > Neutral 2 | 267..358 | 267..358 | 267..358 |
| Rush > Up 2 | 267..358 | 267..358 | 267..358 |
| Neutral 2 > Neutral 3 | 542..633 | 542..633 | 533..625 |
| Neutral 2 > Up 3 | 542..633 | 542..633 | 533..617 |
| Neutral 3 > HP (Square) | 600 (one sample) | 592 | 592 |

Rush > Neutral 2 at 120 Hz was repeated 5 times at 358 ms (Neutral 2 each time) and 367 ms (the repeat move each time). Everything is identical at 60 Hz and within one 8 ms update at 120 Hz.
Not measured: the air chain at 60 / 120 Hz, other fighters (their first update still gets the step, no grid).
