# Training mode (Battle Mode > Training)

Added 25 Nov (tree `tttrain`, branch `tt-training`). A standalone practice mode, independent of the tag team code (`runtime/training.cpp`; nothing in `tagteam*.cpp` was changed).

## Entry and flow
- Battle Mode list: a sixth row **Training** under Tag Team Battle (`menu_settings.cpp`, same mechanism as the Tag Team row: the cursor range is raised, the row drawn on the game layer; `PSPRECOMP_TRAINING_OFF=1` = no row, never active).
  Confirming it sets the Training flag and starts the game's own **Quick Battle (1-on-1)** (cursor put on index 0, a fresh Cross replayed by `dissidia_training_pad`): Player Select with LOOK / SET, CPU Select, STAGE / STRENGTH / LEVEL / TYPE page, retail loading, retail 1v1 battle. Any other confirm in that list clears the flag, so Quick Battle, Tag Team Battle etc. are unchanged (checked: no overlay, no hooks, Select does nothing).
- The game's cursor shows its hand and help text for an index past its table; with the cursor on the Training row the game's update is run with the cursor one lower (the Tag Team row, which it draws nothing for) and its move is translated back (`sub_tick_hook`).
- Active only while the battle overlay runs, no tag battle was chosen and the session is not online (`active()` in training.cpp).

## In the fight
- **No KO end.** The HP step (FUN_08AEFF40, hooked only while a Training battle runs, previous entry restored afterwards) runs with +1,000,000 HP and the damage is capped at HP - 1; a fighter whose HP is at 2 % or less is refilled (the "REFILLED n" counter). HP is Infinite per fighter if chosen.
- **Pause menu** is the retail one (Return to Game / Retry / Forfeit / Settings); Forfeit > YES > retail "Return to Battle Setup / Rematch" is the way out (no KO result screen can happen otherwise). While the pause menu is up the top-left box shows "Select: Training options".
- **Options panel** (Select in the pause menu; the panel takes the pad, Circle closes it, in the menu style of the Battle Lobby via `dissidia_menu_style_list`): CPU ACTION (Normal AI / Stand / Guard), YOUR HP / CPU HP (Refill at 0 / Infinite), YOUR BRV / CPU BRV (Normal / Base value / 0 / 3000 / 9999), EX GAUGE and ASSIST GAUGE (always full, yours), SUMMON (always ready: clears the fighter's use count +0x10CE, not exercised), HIT INFO, RESET POSITIONS (both fighters PLACEd on their start spots, taken when both stand idle after the intro; works while the pause menu is up).
- **Hit info** (top left): last BRV hit (who > who, BRV lost by the victim while the other's BRV rose), last HP hit (damage taken at the HP step, capped), refill count.
- CPU Stand = the brain tick is skipped for CPU brains (framerate.cpp's cpu_tick hook asks `dissidia_training_ai_skip`, after the fight ran 7 s). CPU Guard = skipped brain and the guard action (script 470, started with FUN_0882FA10 like R does for the player) kept running.

## Not done
- **Jump** CPU action: starting action 560 only reaches the take-off script 102 and falls back (the launch needs input the skipped brain no longer gives). **CPU level**: set on the opponent page (STRENGTH / LEVEL / TYPE); not changeable mid-fight. **Partner AI**: the 1v1 has no partner. Summon option and the assist-call behaviour not exercised. A battle time limit (if the chosen rules have one) is not stopped.
- Not tested on a device.

## Tests (host, one copy, `~/dissidia/tt/s2/tttrain/ctl.py`, `walk.sh`, `test1.py`, `test2.py`, `mem.py`)
- Menu: `m1.png` (Battle Mode list), `t2.png` (Tag row), `a1.png` (Training row, clean), `a2.png` Player Select, `b_all.png` Player Select / CPU Select / Enter Battle pages, `c2.png` VS, `c4.png` fight with overlay.
- Options: `d_all.png` / `e_all.png` (pause, panel), `f3.png` (CPU BRV 9999, lethal HP hits: "HP HIT CPU > YOU 2500", REFILLED 12 in 60 s, no KO / result), `j_all.png` (EX / assist full, reset), stand (CPU position unchanged 4 s), guard (script 470 every sample), reset (positions exactly the spots, paused), `x_all.png` / `y_all.png` (Forfeit > YES > Return to Battle Setup > setup page > Rematch: Training again), `z_all.png` (normal Quick Battle: nothing of Training).
- Regression: `PSPRECOMP_CLOUD=0 python3 tools/tagteam_test.py --work ~/dissidia/work_tttrain --baseline --out ~/dissidia/tt/s2/reg_training.223445`: PASS.
