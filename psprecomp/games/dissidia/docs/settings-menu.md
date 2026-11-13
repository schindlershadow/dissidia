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

## For game modules

- `psp_settings_request_open()` (any thread) opens the menu from a game's own menu entry.
- `psp_settings_add_debug_toggle`, `set_frame_rate_hook`, `set_voice_hook`, `set_texpack_hook` as before (`include/psp_settings.h`).

## Status

The in-game entry points (title screen Load, main menu Options, pause menu) are not wired yet: they need the game's own menu code
(overlay patches) found and verified against a real disc image. The runtime side is ready (`psp_settings_request_open()`).
