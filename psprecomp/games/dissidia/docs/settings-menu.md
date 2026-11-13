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

Layout: a category sidebar (Display, Graphics, Image, Effects, Controls (touch screens only), Network, Cloud, System, Drivers (Android),
Debug), the rows of the category (some open a sub-menu: Image > HDR, Effects > Ray Tracing), and a description of the selected row below.
A long description scrolls by itself. The chip next to a description's title says how the choice is kept: **Saved**, **Next start**
(restart the game to apply) or **This session**.

Text fields (server, room, sync folder, WebDAV) open an on-screen keyboard: pad, mouse or touch, or just type on a keyboard.

## Categories, restart and mods

The menu opens on the category list (left): Up / Down choose, Confirm or Right go in, Back returns (Back on the list closes). L / R and Q / E
still switch category from anywhere. The list scrolls when there are more categories than fit.

Changes marked **Next start** ask "Restart now / Later" when the menu is closed. Restart runs the launch command `run.sh` exports in
`PSPRECOMP_RESTART_CMD` (so mods are built again); without it the same program and arguments start again. Not on Android / Windows (OK only).

The **Mods** category lists mod folders with a `mod.json` (the repo's `mods/`, `PSPRECOMP_MODS_SRC`, and the game's mods folder) and extra
characters from the mods folder's `roster.txt`. A switch writes `"enabled"` into `mod.json`, or puts `#off# ` in front of the roster line.

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

The in-game entry points (title screen, main menu Options, battle pause) are not wired. The runtime side is ready
(`psp_settings_request_open()`). What was found: the title's NEW GAME / LOAD GAME / EXTRAS cursor is the word at 0x096AE20C
(0, 1, 2; heap object, address may move), written by EBOOT code under 0x088DA0A8 (the generic data-driven list engine, shared with
other menus), and the labels are game art, so a fourth row needs a new sprite and layout plus the engine's item table, not just a
hook.
More detail (run with `PSPRECOMP_TRACE_CALLS=088DA0A8`): the title's three rows are static sprite objects in the overlay data area
(object list at 0x08B8C1C8..0x08B8C360, a0 = 0x08B8C31C for the menu), queried once per row and frame as `FUN_088DA0A8(menu, index,
...)` with index 0, 1, 2. Next step for a Settings row: add a fourth object (clone of the EXTRAS sprite plus a "SETTINGS" label drawn
by the runtime), raise the row count and intercept the confirm for index 3 to call `psp_settings_request_open()`. The title idles into
the attract movie after about 60 s, so probe in one scripted pass.
