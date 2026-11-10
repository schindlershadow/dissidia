# Save data and moving saves between regions

Saves are PSP save folders in `<workdir>/ms0/PSP/SAVEDATA/`, named `<disc ID><suffix>`:

| Folder | Content |
|---|---|
| `ULUS10566GameData00` | the game data (player, progress); the same on every disc, only the ID differs |
| `ULUS10566FCard00` .. | friend cards (the ghost data of other players) |
| `ULUS10566Quest00`, `...Replay00` | quest and replay saves |

Each holds `PARAM.SFO`, `ICON0.PNG` and the game's encrypted files (`DISSIDIA.BIN`, `FCARD.BIN` ...). The files are
the same for every region (the key and the format are shared); a disc only looks for the folder with its own ID. The
disc IDs seen so far: `ULUS10566` (US), `ULES01505` (Europe). A Japanese disc has its own `ULJM...` ID, which
`savemgr` takes from the install, or from `--id`.

## One save set for every region

The runtime treats `ULUS10566`, `ULES01505` and `ULJM05681` as the same game (`psp_savedata_set_shared_ids`, called
from the Dissidia module; more IDs can be added with `PSPRECOMP_SAVE_IDS=ID1,ID2`). When the game asks for
`<its ID><slot>`, the runtime uses whichever region's folder exists, so a US save loads on an EU install with no copying
or renaming, and the save list shows each slot once. Saving writes back into the folder that was found; a slot that
exists nowhere is created under the running disc's own ID. Test: `runtime/tests/test_savedata_hle.cpp`.

## In-game: Manage saves

Open the game's save list (Load Game / Save Game) and press **Triangle** (the hint is in the bottom bar; with no saves
yet the list says so and still offers it). The **Manage Saves** screen has:

- **Import saves**: lists the places a Dissidia 012 save may be: PPSSPP's `PSP/SAVEDATA` (Linux `~/.config/ppsspp`,
  Flatpak, macOS, Windows `Documents\PPSSPP` and `%APPDATA%`), PSP memory sticks mounted under `/media`, `/run/media`,
  `/mnt` (two levels) and Windows drive letters, with the number of saves in each; or **Type a path...** (host
  keyboard, or the pad keyboard; any folder holding save folders, or a memory stick / PPSSPP root). Then pick saves
  (**All saves** toggles the default set: one save per slot, this game's region first; Cross toggles a save,
  Start imports) and **Import selected**. Saves go in under this disc's ID (or into the other region's folder if the
  game already uses one, see above), `PARAM.SFO` signed again as `savemgr` does.
- **Export saves**: the same in reverse: pick PPSSPP / a memory stick / a typed folder, pick saves, choose
  **Folder names**: *keep region* or *as ULES01505* (this game's ID, so a matching PPSSPP game finds them).

Nothing is replaced silently: when a save already exists you choose **Replace them** (the old copy is moved to
`ms0/PSP/SAVEDATA_BACKUP/<name>.bak`, never inside `SAVEDATA`, which the game lists) or **Skip existing**. Results are
listed per save and logged as `[SAVEMGR]`. `PSPRECOMP_SAVEMGR=0` removes the Triangle key. The logic lives in
`runtime/src/hle/psp_savemgr.cpp` (shared with `tools/savemgr`) and the screen in `psp_savemgr_ui.cpp`; tests:
`runtime/tests/test_savemgr.cpp` (synthetic folders, every flow driven by pad presses, and the real PPSSPP folder
read-only when `~/.config/ppsspp/PSP/SAVEDATA` exists; `PSPRECOMP_TEST_PPSSPP_SAVES` overrides it).

## savemgr

`tools/savemgr/savemgr.cpp` is a small stand-alone tool (C++17, no libraries beyond `psp_savedata_crypto`):

```
g++ -O2 -std=c++17 -Iruntime/src/hle tools/savemgr/savemgr.cpp runtime/src/hle/psp_savemgr.cpp runtime/src/hle/psp_savedata_crypto.cpp -o savemgr

savemgr list   [--from DIR]
savemgr import --from DIR [--workdir DIR | --to DIR] [--id ULUS10566] [--force] [NAME...]
savemgr export [--from DIR] --to-dir DIR [--id ULES01505] [NAME...]
```

- `DIR` is a `SAVEDATA` folder (a PSP memory stick's `PSP/SAVEDATA`, PPSSPP's, or another install's), or one save folder.
  Only Dissidia 012 saves are used (by the `TITLE` in `PARAM.SFO`); other games' saves and the first Dissidia's are
  ignored. `NAME` picks saves: a folder name (`ULES01505GameData00`) or a suffix (`GameData00`); none means all.
- `import` copies the saves into the install's `ms0/PSP/SAVEDATA` (`--workdir`, default the current folder) under **the
  installed disc's ID**, read from `disc0/PSP_GAME/PARAM.SFO`. An existing save is never replaced unless `--force`,
  which keeps the old one as `<name>.bak`.
- `export` copies saves to a folder, keeping their region, or giving them `--id`'s.
- Only the files `PARAM.SFO` lists (plus the icon files) are copied: a stray `*.sync-conflict-*` file from a file-sync
  tool stays behind.

What changes in a copy: the folder name, `SAVEDATA_DIRECTORY` in `PARAM.SFO`, and the signature in `SAVEDATA_PARAMS`,
made with the same code PPSSPP uses (`psp_savedata_crypto`). A copy that keeps its folder name is byte-identical.
The runtime and PPSSPP accept the result. **A real PSP does not**: it checks `PARAM.SFO` against its own console key,
which cannot be made here (sign it with a tool such as Savegame Manager Deluxe, or use the game's own save).

Test: `tools/savemgr/test_savemgr.sh` (synthetic saves, no game data needed).
