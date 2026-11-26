# Save data and moving saves between regions

Saves are PSP save folders in `<workdir>/ms0/PSP/SAVEDATA/`, named `<disc ID><suffix>`:

| Folder | Content |
|---|---|
| `ULUS10566GameData00` | the game data (player, progress); the same on every disc, only the ID differs |
| `ULUS10566FCard00` .. | friend cards (the ghost data of other players) |
| `ULUS10566Quest00`, `...Replay00` | quest and replay saves |

Each holds `PARAM.SFO`, `ICON0.PNG` and the game's encrypted files (`DISSIDIA.BIN`, `FCARD.BIN` ...). The files are
the same for every region (the key and the format are shared); a disc only looks for the folder with its own ID. Disc IDs:
`ULUS10566` (US), `ULES01505` (Europe), `ULJM05814` (Japan, UMD), `NPJH50377` (Japan, PSN).

## One save set for every region

The runtime treats `ULUS10566`, `ULES01505`, `ULJM05814`, `NPJH50377` (and `ULJM05681`, listed before the Japanese IDs were checked)
as the same game (`psp_savedata_set_shared_ids`, called from the Dissidia module; more IDs can be added with
`PSPRECOMP_SAVE_IDS=ID1,ID2`). When the game asks for `<its ID><slot>`, the runtime uses whichever region's folder exists, so a US
save loads on an EU install with no copying or renaming, and the save list shows each slot once. Saving writes back into the folder
that was found; a slot that exists nowhere is created under the running disc's own ID. Test: `runtime/tests/test_savedata_hle.cpp`.

## Settings > Saves (the save manager)

**Settings > Saves**, or **Triangle** on the game's save list (Load Game / Save Game; the hint is in the bottom bar and says when cloud
saves wait for a choice). It is a category of the Settings menu, so the pad, the keyboard, the mouse and touch all work; the game is
paused while it is open and the save list behind it is refreshed when it closes.

- **Needs your choice** (only when cloud sync found a save changed on two devices): one screen per save with both sides - this device
  and the cloud, the device that uploaded it by name, when each was saved (the newer one marked) and the save's own description - and
  *Keep this device's save* / *Use the cloud save* / *Decide later*; *Keep the newer save of each* settles all of them. The side that
  loses is never lost: this device's save goes to Backups, the cloud's is kept in the cloud as `<slot>.bak`.
- **Cloud sync** (at the top, when cloud sync runs): status, *Sync now*, and a jump to Settings > Cloud.
- **This game's saves** (*Duplicate, move, rename or delete...*): pick one of your saves, then
  - *Duplicate to another slot...*: a copy in another slot (the game's save list offers game data slots 00 to 19 and lists every one
    that exists), e.g. to keep a copy from before a hard fight; a taken slot is asked about and its save goes to Backups;
  - *Move to another slot...*: the same, and the old slot becomes free (its folder goes to Backups);
  - *Rename...*: the name the save list shows (`SAVEDATA_TITLE`, e.g. "GAME DATA"), to tell copies apart; the game writes its own name
    back the next time it saves into that slot;
  - *Delete*: into Backups (restorable). With cloud sync on, the slot is deleted in the cloud too (a deletion marker; the cloud copy is
    kept once as `<slot>.bak`), and each other device moves its copy into its own backups at its next sync - unless it changed that
    save since, in which case the save stays and is uploaded again.
- **Import saves**
  - *From PPSSPP / a memory stick*: found automatically (PPSSPP's `PSP/SAVEDATA` on Linux, Flatpak, macOS, Windows; memory sticks and
    disks under `/media`, `/run/media`, `/mnt` and Windows drive letters), with the number of saves in each. The search runs in the
    background (a sleeping disk can take seconds).
  - *Choose a folder...* / *Choose an archive...*: the system's file dialog (Windows' own, zenity / kdialog / matedialog / qarma on Linux,
    macOS; Android's document picker). An archive (.zip, .tar, .tar.gz built in; .7z, .rar, .tar.xz through 7-Zip / bsdtar / unrar when
    installed) is unpacked into `savemgr_tmp/` in the game folder and removed afterwards; entries that would land outside it are refused.
  - *Browse in the game...*: an in-game file browser (places: the game folder, home, Downloads, Documents, drives), for setups without a
    file dialog (Steam Deck game mode, an Android build without the picker).
  - *Type a path...*: a folder or an archive.

  Saves are found **anywhere below** the picked place (a `PSP/SAVEDATA` folder, one save folder, a folder of saves, or a save's files
  with any folder name: the name then comes from `PARAM.SFO`'s `SAVEDATA_DIRECTORY`), from every region; Dissidia 012 saves are
  recognised by their disc ID or by the English or Japanese title, other games' saves (and the first Dissidia's) are left out. Then
  pick saves: one per slot (this game's region first, then the newest); each row's description says whether it replaces one of yours
  and which is newer. They go in under this game's folder name (or the other region's, if the game already uses that), `PARAM.SFO`
  signed again as `savemgr` does.
- **Export saves**: *Export to a folder...* or *Export as a .zip file...* (the system's dialog), PPSSPP / a memory stick, the in-game
  browser, or a typed path (a name ending in `.zip` writes a zip). A memory stick or PPSSPP folder gets the saves in its `PSP/SAVEDATA`;
  a zip holds them under `PSP/SAVEDATA/`. **Folder names**: *Keep region* or *As ULES01505* (this game's ID).
- **Backups** (*Back up now*, *Restore a backup*), see below.

Nothing is replaced silently: when a save already exists you choose *Replace* or *Only new ones*. Results are logged as `[SAVEMGR]`.
`PSPRECOMP_SAVEMGR=0` removes the Triangle key; `PSPRECOMP_FILE_DIALOG=none` always uses the in-game browser instead of the system's
dialog.

## Opening Settings > Saves is not read on the UI thread (26 Nov)

Opening the category used to read every save and every backup folder on the UI thread (`psp_savebackup::list` parses each save of each
backup: with a long history that is thousands of files, and on a phone's shared storage (FUSE) each costs a millisecond or more, so the
screen froze for seconds). Now `Page::reset()` starts a background load (this game's saves and the backup list); the top level shows
"Loading..." as the value of *This game's saves* / *Restore a backup* until it is back, and *Restore a backup* shows "Loading backups..."
the same way, so the menu stays drawn and usable (the Import rows already filled in from their own thread). `psp_savebackup::list` also
keeps what it read of each backup folder (keyed by the folder's modification time; a folder younger than 3 s is not kept), so opening again
does not read the backups again, and `read_save` uses one `stat` instead of two. Host numbers (`tests/bench_saves_open.cpp`, 20 saves,
54 backups of 20 saves): UI thread 28 ms -> 0.1 ms, 37 000 -> 16 000 file system calls for the first open. `PSPRECOMP_SAVES_SYNC_OLD=1`
restores the old behaviour (read on the UI thread, no cache).

## Backups

Every save that is about to be replaced - by a cloud download, by *Use the cloud save* in a conflict, by an import or by a restore - is
moved into a dated backup first, and once a day (the first start of a day, before the start-up sync) every save is copied while
they have changed. They live in `ms0/PSP/SAVEDATA_BACKUP/` (never inside `SAVEDATA`, which the game lists):

| Folder | What |
|---|---|
| `20261119-210405_cloud-download/ULES01505GameData00/` | one backup: local date and time, the reason (`cloud-download`, `cloud-conflict`, `import`, `restore`, `daily`, `manual`, `deleted`, `moved`, `renamed`, `replaced`), the save folders as they were |
| `ULES01505GameData00.bak` | an older runtime's single backup: listed as "Older backup" |

Saves replaced by one occasion within two minutes (a sync that downloads three saves) share one backup. The newest 40 automatic and
14 daily backups are kept; *Back up now* backups stay until deleted. **Restore** puts a backup's save back (the current one is backed up
first, so a restore can be undone too); with cloud sync on, the restored save is uploaded at once, so rolling back a bad cloud download
also fixes the other devices.

The logic lives in `runtime/src/hle/psp_savemgr.cpp` (regions, finding, copying, shared with `tools/savemgr`), `psp_savebackup.cpp`,
`psp_savearchive.cpp`, `psp_saves_page.cpp` (the page) and `runtime/src/psp_file_dialog.cpp` (the system's dialog, through the installer's
portable-file-dialogs; Android: `android/game_java/.../FilePicker.java`). Test: `runtime/tests/test_savemgr.cpp` (synthetic saves in every
layout, archives in and out and refused ones, backups, the page driven like the menu with a fake file dialog and a fake cloud, and the
real PPSSPP folder read-only when `~/.config/ppsspp/PSP/SAVEDATA` exists; `PSPRECOMP_TEST_PPSSPP_SAVES` overrides it; `DUMP_SAVES=/dir`
writes pictures of the page).

## savemgr

`tools/savemgr/savemgr.cpp` is a small stand-alone tool (C++17, no libraries beyond `psp_savedata_crypto`):

```
g++ -O2 -std=c++17 -Iruntime/src/hle tools/savemgr/savemgr.cpp runtime/src/hle/psp_savemgr.cpp runtime/src/hle/psp_savebackup.cpp runtime/src/hle/psp_savedata_crypto.cpp -o savemgr

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
