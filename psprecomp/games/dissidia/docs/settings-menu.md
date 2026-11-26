# Settings menu

Open it with **Esc** / **F1** (keyboard), the **Guide** button or **L3 + R3** (pad), or the menu button at the top of the touch controls.
The game pauses while it is open.

Controls

| Input | What it does |
|-------|--------------|
| Pad: d-pad / left stick | Up / down choose a row, left / right change its value |
| Pad: Cross / Circle | Select / Back (up one level, then close) |
| Pad: L / R | Previous / next category |
| Keyboard | Arrows, Enter (select), Esc or Backspace (back), Q / E or Tab (category) |
| Mouse | Hover highlights, click selects, the arrows and slider bars are clickable and draggable, wheel scrolls, right click = Back |
| Touch | Tap a row, drag the list to scroll (it coasts), slide on a bar to adjust, tap a category on the left; the on-screen game controls are hidden |

Layout: a category sidebar (Display, Graphics, Image, Effects, Controls (touch screens only), Network, Saves (the save manager:
[savedata.md](savedata.md)), Cloud, Mods, System, Drivers (Android), Debug), the rows of the category (some open a sub-menu: Image > HDR, Effects > Ray Tracing), and a description of the selected row below.
A long description scrolls by itself. The chip next to a description's title says how the choice is kept: **Saved**, **Next start**
(restart the game to apply) or **This session**.

**Controls > Button Prompts** (Automatic, PSP, Xbox, PlayStation, Nintendo, Keyboard; saved as `button_prompts`,
`PSPRECOMP_BUTTON_PROMPTS`): the game's button symbols drawn as the buttons you press. Automatic follows the device player 1 used
last: a PlayStation pad's shapes, a Nintendo pad's letters, Xbox letters for any other pad (a Steam Deck's A B X Y with L1 / R1), the
keyboard's keys (on the game's silver key caps) after a key press, the PSP's own with neither. The face symbol shown is the one of the
button that sends the PSP's button (`PSPRECOMP_SWAP_AB` and a Nintendo pad's label layout move them). The game draws its prompts
from nine textures; `games/dissidia/runtime/button_prompts.cpp` redraws them at four times their size as they load (the drawing is
VCKing101's, MIT), through the runtime's texture editor (`psp_texpack_set_editor`, keyed by PPSSPP's xxh64 dump keys, which a texture
pack's own image for the same texture overrides). `PSPRECOMP_NO_TEXTURE_EDITS=1` keeps the PSP's symbols, `PSPRECOMP_PROMPTS_LOG=1`
logs each redraw. The Controls page is now on every device; its Touch screen rows only with a touch screen.

**Controls > Button Chord Window** (Off, Short, Medium; `chord_ms`, `PSPRECOMP_CHORD_MS=<ms>`, default Short = 70 ms, Medium = 110 ms, 0 = off):
in battle a face button pressed with no L / R held is held back for the window; if L or R comes down in it both reach the game in one
pad read (R + Cross dodge, R + Triangle dash, R + Square EX, R + Circle summon, L + Circle / L + Square assists), otherwise the button
arrives that much late (a tap shorter than the window is still delivered, for 34 ms). See frame-rate.md "Button chord window".

**Controls > L / R Buttons** (Shoulders, Triggers; `lr_buttons`, `PSPRECOMP_LR_TRIGGERS=1`): Shoulders (the default) is as before,
the shoulder buttons press L and R and so do the triggers. Triggers: only the triggers press L and R, the shoulders nothing, and the
prompts name the triggers (LT / RT, L2 / R2, ZL / ZR). **Player 1 Controller** (`p1_controller`, `PSPRECOMP_P1_CONTROLLER=any|keyboard|<GUID>`):
Any Controller (the default), one connected controller by name (the others are ignored: a Steam Deck with a pad plugged in, a pad
someone else holds), or Keyboard Only. A saved controller that is not connected stays a choice, marked "(not connected)".
**Controls > Test Controls** (generic runtime, `psp_settings.cpp` / `psp_settings_render.inc`; no setting is saved): a full-screen PSP
drawn with shapes (no image assets): D-pad, Cross / Circle / Square / Triangle, L, R, Start, Select and the analog stick, each lit
while the PSP input reads it pressed, the stick nub following the live position, and the raw stick values (0..255, 128 = centre) with the
list of pressed buttons on its screen. It shows the input after the game's mapping: keyboard (move keys press two buttons), controllers
(Player 1 Controller, L / R Buttons, A / B swap), the on-screen touch controls (they stay live over the screen; Start + Select touches
leave) and, for player 1, debug-socket injections. A tab per player: Player 1 is what the game reads for player 1; Player 2 is a
placeholder for split screen that reads the second connected controller (the first one Player 1 does not use; "No controller" if none).
The **Controller** panel shows the same player's physical controller (SDL game controller) raw, before the game's mapping: left and right
stick (live nub, raw -32768..32767 values, L3 / R3 click lights the ring), L2 / R2 as analogue bars (0..100 %, "DOWN" past the press
threshold) and the other buttons by SDL name (A B X Y, LB RB, Back / Guide / Start, D-pad), lit while down. Each control states what the game
does with it, read from the runtime's own mapping: L2 / R2 follow `PSPRECOMP_PAD_L2` / `_R2` (the game's run.sh sets L2 = R + Cross) or become
the PSP L / R when Controls > L / R Buttons = Triggers (LB / RB then do nothing); the A / B swap shows in A / B; R3 is the camera re-center; the
right stick is the PSP right analog the game reads. A line lists what is down as "button > game action". Keyboard-only / touch players show
"No controller for this player". On a tall canvas (design height >= 380) the PSP and the panel are stacked; on 16:9 two tabs (PSP /
Controller, click, touch or F3) switch between them. Test rigs: `PSPRECOMP_TEST_VPAD="rx,ry,lt,rt,buttonmask,lx,ly"` attaches an SDL virtual
controller holding those raw values (buttonmask bit i = SDL_GameControllerButton i), `PSPRECOMP_TEST_OPEN_TESTCONTROLS=<s>` opens the screen
by itself after that many seconds and `PSPRECOMP_TEST_TESTCONTROLS_VIEW=1` starts on the Controller tab.
Switch with a click or touch on a tab, PageUp / PageDown / F2, or by holding L + R for 1 s on a pad; on 16:9 the PSP / Controller views switch with a tab click or touch, F3, or Select + R held 1 s (pad). Leave with Esc, F1, Android Back,
the Exit button (click / touch), right click, or by holding Start + Select for 2 s (from any player's input; a bar shows the hold); all
other keys and buttons are the buttons under test and do nothing to the menu. `psp_input_sample(player, &sample)` (`psp_event_loop.h`)
is the API a split-screen mode can reuse; the pre-menu-gate state is kept by `publish_input` even while the menu hides it.
**Keyboard > Keyboard Controls** (a row that opens a list page: Cross opens, Back returns; two-button moves first, then the other keys): one key for each move that takes two buttons, both going down in the same pad read as the game needs
(R held first makes a block instead): D dodge (R + Cross), F dash (R + Triangle), E EX Mode (R + Square), V summon (R + Circle),
1 Brave assist (L + Circle), 2 HP assist (L + Square). Holding R or L and pressing the face button still works. (VCKing101's idea,
`docs/port/keyboard-controls.md`; our keys, as our keyboard layout is fixed.) `PSPRECOMP_KEY_LOG=1` logs every change of the buttons
the game reads.

Text fields (server, room, sync folder, WebDAV) open an on-screen keyboard: pad, mouse or touch, or just type on a keyboard. Its **Paste** key (or Ctrl+V) types the clipboard's text in: printable characters only, line breaks dropped, up to the field's length. The game's own name entry (Data Config > Player Name) has a Paste key and Ctrl+V too.

## Categories, restart and mods

The menu opens on the category list (left): Up / Down choose, Confirm or Right go in, Back returns (Back on the list closes). L / R and Q / E
still switch category from anywhere. The list scrolls when there are more categories than fit.

Changes marked **Next start** ask "Restart now / Later" when the menu is closed. Restart runs the launch command `run.sh` exports in
`PSPRECOMP_RESTART_CMD` (so mods are built again); without it the same program and arguments start again. Not on Android / Windows (OK only).

The **Mods** category lists mod folders with a `mod.json` (the repo's `mods/`, `PSPRECOMP_MODS_SRC`, and the game's mods folder) and extra
characters from the mods folder's `roster.txt`. A switch writes `"enabled"` into `mod.json`, or puts `#off# ` in front of the roster line.
The page has one Install row per installer the game offers: an `.xdelta` ISO patch, and the DLC from a folder or from a .zip (below).

**DLC** (issue 134). Dissidia 012's downloadable content (costumes, music, Aerith's unlock key) is a folder of NPDRM EDAT files named by the
8 hex digits of the asset's name hash (`PSP/GAME/ULUS10566/eac7bf53.edat` in PPSSPP or on a memory stick). The game opens each as
`ms0:/PSP/GAME/<game id>/<hash>.edat` with the NPDRM flag, calls `sceNpDrmEdataSetupKey` on it and reads the plain data. The runtime
decrypts an EDAT (`"\0PSPEDAT"` + PGD, `psp_npdrm.cpp`, ported from PPSSPP's amctrl) when the game sets the key; a file without the magic is
plain data and is read as it is. Install DLC from a Folder / from a Zip (Settings > Mods, or the installer's Import DLC row, which finds
PPSSPP and memory sticks itself) copies the files into the mod folder `mods/dissidia-dlc/dlc/` (`mod.json` with `"kind": "dlc"`), which shows
in the list below with its switch (applies at the next start; with every DLC folder off the game finds no DLC). Files copied by hand into
`ms0/PSP/GAME/<id>/` are found too, by name without regard to case, when there is no DLC mod folder. `PSPRECOMP_DLC_IMPORT=<folder or .zip>`
installs one at start and exits (tests). The async open of an EDAT leaves its completion callback queued instead of running it inside
`sceIoSetAsyncCallback`: the game stores the fd that `sceIoOpenAsync` returned only after that call returns, and the callback reads it back
(running it in place made it see fd -1; the EDAT was then never keyed and its encrypted bytes were parsed as data).

The **Network** category also has a **Discord** section (not on Android): **Discord Status** On / Off (applies at once, saved) and a
connection line; see [discord.md](discord.md).

The screen is drawn by up to 8 threads in horizontal bands (`PSPRECOMP_SETTINGS_THREADS=N` to change, 1 = one thread);
`PSPRECOMP_SETTINGS_PROFILE=1` logs the drawing time per part.

## For game modules

- `psp_settings_request_open()` (any thread) opens the menu from a game's own menu entry.
- `psp_settings_add_debug_toggle`, `set_frame_rate_hook`, `set_voice_hook`, `set_texpack_hook` as before (`include/psp_settings.h`).

## Status

Verified in a real run (Xvfb, NVIDIA GL, 2026-11-13): opens with Esc, category switch (Q/E and mouse), toggles and sliders write
`settings.ini`, the Network on-screen keyboard (typing, Esc cancels), mouse click / wheel in a letterboxed 1200x500 window, close
returns to the game. Not verified: pad, touch / Android, HDR and Ray Tracing sub-menus (the host GPU reports both Unavailable),
the ~60 fps animation / scale auto-drop (no frame timing taken), held-button leak after close.

## In-game entries (games/dissidia/runtime/menu_settings.cpp)

"Settings" is a row of the title screen, the start menu and the battle pause menu; choosing it opens this screen
(`psp_settings_request_open()`). `PSPRECOMP_MENU_SETTINGS=0` removes the rows.

- **Battle pause menu**: a fourth (or later) item of the game's own list widget, added just before the list opens (battle overlay
  FUN_08B1EBE8 builds the list, EBOOT FUN_088D72E8 adds, FUN_088D743C opens; up to 7 items). Its label and help text are served
  for marker ids by a hook of the text accessor FUN_0887AD78, so bars, font, cursor, help line and sounds are the game's. A
  confirm on it is taken back in the selection core's update (FUN_088D80AC). Not added during a wireless match.
- **Start menu**: the seven retail rows are pulled 21 px apart (an in-flight edit of entry 0 of
  `menu/menu_pk_upper_start_menu_seq_0.bin`, both keyframes of each row node), the cursor range of the top level is raised to 7, and
  the eighth row is drawn on the game layer (`ge_draw_set_game_layer`, `menu_layer.cpp`): the game's bold text face, the
  measured line / band / help-line colours and the pointing hand of the common UI sheet, following the Options row's live
  position. A confirm on index 7 is handled before ovl_menu FUN_089E45E8 reads its 7-entry mode table.
- **Title screen**: NEW GAME / LOAD GAME / EXTRAS and the bar nodes move up 16 (entry 0 of `menu/en/menu_lpk_title_seq_0.bin`)
  and Settings takes EXTRAS' place. The game keeps three rows; "Settings selected" keeps its cursor on EXTRAS and moves the game's
  own bar and hand (the selection instance, whose origin is the selected row's node) onto the fourth row, and Up / Down in and
  out of it are handed to the game's own moves so its sounds and animations play. The label is drawn in Crimson Text SemiBold
  small capitals; its underline is a copy of another row's underline from the game's own picture (`ge_draw_set_game_layer_copies`).

**Texture Pack** (Graphics): Off or one of the installed packs, applied at the next start; **Download Dissidia Recomp HD** (while it is not
installed); **Install a Texture Pack** (a .zip, on a PC also .7z / .rar,
with the file picker) and **Delete a Texture Pack** (not the one in use);
see [texture-packs.md](texture-packs.md) "Several packs, one applied".

**HD texture packs.** The added rows take their look from what the game really draws, so they follow both packs (our id-named
upscales and PPSSPP-style packs a player installs): the runtime keeps the image a watched texture was replaced with
(`psp_texpack_watch`), and the rows use the HD pointing hand of the common UI sheet (id `c6f05e9985eebe31`) and, on the title, the
lettering colour and size of the labels on the HD title sheet (id `585ca08f8a5250d3`; e.g. white and about 0.6x on a pack with a
black title). The title bar, hand and underline are the game's own pixels. The pause menu row is the game's own and needs nothing. Ids
of retail textures: `tools/texpack_extract.py`.

A loose-file mod that replaces one of the two layout files keeps its own layout (the built-in edit is skipped), and the start
menu / title rows then overlap the retail rows; the pause menu row does not depend on files.

## Debug page notes (21 Nov)
- `psp_settings_add_debug_toggle` has no small cap any more (it was 10: the Jecht Combo Timing row, registered third, was dropped once the tag team added rows). The rows are a range of 256 row ids (`ROW_DBG0..ROW_DBG_END`, ids are per-row animation slots, `SET_MAX_IDS` = 1024) and the page scrolls.
  lists a row right below the FX Shaders row; the Jecht Combo Timing helper uses it.
- Controls > L / R Buttons (Shoulders / Triggers) is a value selector (< value >), not an On / Off switch.
- Controls > Keyboard Controls opens the Keyboard Controls sub-page (read-only lines: key, move, PSP buttons).
- Debug > **Button Chord Window (ms)** (Off, 10 to 200 ms in steps of 10): the fine version of Controls > Button Chord Window, the same saved `chord_ms` (default 70), applied at once. The Controls row shows "Custom (N ms)" when the value is none of its presets (Off / Short 70 / Medium 110).
- `PspDebugToggle` has `def` (the value Reset All sets) and `group`: rows with the same group name are listed on a submenu row of the Debug page (Cross opens, Back returns; the row shows "N of M on"). The Dissidia tag team rows use group "Tag Team". Generic code only: `g_dbg_groups`, `PAGE_DBGGRP`, `ROW_GRP0..`.
- Debug > **Reset All** (last row, asks Yes / No, No pre-selected): Performance Overlay off, FX Shaders on, GPU Sync off, Vertex Upload Sub, Glow Softening / Passes on, Effect Light sliders 2.5 / 1.5, chord window 70 ms, every game toggle to its `def`, and every developer option back to its default (those apply at the next start). Saved.

## Game-module saved values, Tag Team debug rows are test overrides (22 Nov)
- `psp_settings_saved_value(key)` / `psp_settings_save_value(key, value)` (psp_settings.h): a game module reads / writes a settings.ini key of its own (what the file held at the last read or save; writing rewrites the file). Dissidia uses
  `tagteam_lobby=mode,size,fill,strength,stage,rules,judgement,exb,assist` for the battle lobby's last choice.
- Debug > Tag Team: the mode rows (Tag Team (2v2), 3v3, 2v1, 1v2, Free-for-all) are test overrides now: the Battle Lobby (Select on a row of the Tag Team Battle setup screens, Battle Mode and Communications) decides the mode;
  a Debug row / env variable that is on wins over the lobby, and "Tag Team (2v2)" on makes every battle a tag battle (default off: plain Quick Battle / Group Battle are retail). See tag-team.md "Battle lobby in single player".
