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

**Controls > L / R Buttons** (Shoulders, Triggers; `lr_buttons`, `PSPRECOMP_LR_TRIGGERS=1`): Shoulders (the default) is as before,
the shoulder buttons press L and R and so do the triggers. Triggers: only the triggers press L and R, the shoulders nothing, and the
prompts name the triggers (LT / RT, L2 / R2, ZL / ZR). **Player 1 Controller** (`p1_controller`, `PSPRECOMP_P1_CONTROLLER=any|keyboard|<GUID>`):
Any Controller (the default), one connected controller by name (the others are ignored: a Steam Deck with a pad plugged in, a pad
someone else holds), or Keyboard Only. A saved controller that is not connected stays a choice, marked "(not connected)".
**Keyboard > Move Keys**: one key for each move that takes two buttons, both going down in the same pad read as the game needs
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
