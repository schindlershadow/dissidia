# Dissidia 012 Duodecim Recompiled

Dissidia 012 Duodecim (PSP) recompiled into a native PC and Android game: no emulator. It runs at up to 240 fps, with HD textures,
ray-traced lighting and reflections, Japanese voices, cloud saves, online multiplayer and its own modding tools for new characters.

You need **your own copy of the game** as an `.iso` or `.cso` file. Nothing from the game is included here or in the downloads: the
installer builds the game on your device from your disc.
<img width="3411" height="1233" alt="image-4" src="https://github.com/user-attachments/assets/ef26636b-42f1-4121-8e01-2cc1fe99d479" />

## Install

Download **one** file for your device. These links always point at the newest release.

| Your device | Download |
|---|---|
| **Windows** PC | [DissidiaRecomp-Setup-windows-x86_64.exe](https://github.com/schindlershadow/dissidia/releases/download/v0.1.23/DissidiaRecomp-Setup-0.1.23-windows-x86_64.exe) |
| **Linux** PC / **Steam Deck** | [DissidiaRecomp-Setup-linux-x86_64](https://github.com/schindlershadow/dissidia/releases/download/v0.1.23/DissidiaRecomp-Setup-0.1.23-linux-x86_64) |
| **Android** phone / handheld | [DissidiaRecomp-Setup-android.apk](https://github.com/schindlershadow/dissidia/releases/download/v0.1.23/DissidiaRecomp-Setup-0.1.23-android.apk) |

Then:

1. **Run it.**
   - Windows: double-click the `.exe`. If SmartScreen warns you, click **More info > Run anyway**.
   - Linux / Steam Deck (Desktop Mode): right-click the file > Properties > Permissions > tick **Is executable**, then double-click it.
     Or in a terminal: `chmod +x DissidiaRecomp-Setup-linux-x86_64 && ./DissidiaRecomp-Setup-linux-x86_64`
   - Android: open the `.apk` and allow your browser or file manager to install apps when asked.
2. **Pick your game file** (`.iso` or `.cso`) when the setup asks for it.
3. **Choose your extras** (all optional): HD textures, Japanese voices (needs an undub disc image), importing saves, and **Add to Steam**.
4. **Wait while it builds.** It translates your copy of the game and compiles it: a few minutes to half an hour depending on the machine.
   Keep the device plugged in and awake.
5. **Play.** Use the shortcut the setup made (Start Menu, application menu, Steam or your Android home screen).

- **Frame rates** above the PSP's 30: any multiple of 30 up to 240, with game logic, UI and EX Burst timing scaled to match ([docs/frame-rate.md](psprecomp/games/dissidia/docs/frame-rate.md)).
- **HD textures** from community packs (on by default when present) and **undub audio** (Japanese voices and movies extracted from an undub disc) ([texture packs](psprecomp/games/dissidia/docs/texture-packs.md), [audio](psprecomp/games/dissidia/docs/audio.md)).
- **Cloud save sync** between devices through a folder, WebDAV or Google Drive ([docs/cloud-sync.md](psprecomp/games/dissidia/docs/cloud-sync.md)).
- **Discord Rich Presence**: your Discord profile shows the mode, the fighters and the stage ([docs/discord.md](psprecomp/games/dissidia/docs/discord.md)).
- **Cross-region saves** and **internet multiplayer** (below).
- A headless battle test harness for checking moves at different frame rates ([docs/testing.md](psprecomp/games/dissidia/docs/testing.md)).

Something went wrong? The setup writes `setup.log` into the install folder: attach it to a
[bug report](https://github.com/schindlershadow/dissidia/issues).

All releases and their change notes: [Releases page](https://github.com/schindlershadow/dissidia/releases).

### Which game file works

The European PSN release (`ULES01505`) is the one the project is built against; the US UMD (`ULUS10566`) works too. `.iso` and `.cso`
are read directly. Other formats (ZSO, DAX, `.bin`, PSN `.PBP`) need converting to `.iso` first, for example with
`maxcso --decompress`.

## Features

- **High frame rates**: any multiple of 30 up to 240 fps, with game logic, UI and EX Burst timing scaled to match
  ([details](psprecomp/games/dissidia/docs/frame-rate.md)).
- **Any resolution and screen shape**: render scale, widescreen, ultrawide and tall / folding-phone screens with the HUD moved to the edges.
- **HD textures**: the Dissidia Recomp HD pack (downloaded in the setup), plus your own texture pack files (.zip; on a PC also .7z / .rar); install several and switch between them in Settings
  ([texture packs](psprecomp/games/dissidia/docs/texture-packs.md)).
- **Better lighting**: ray-traced shadows and reflections (Vulkan with a ray-tracing GPU), attacks that light up the stage, SSAO, HDR and anti-aliasing.
- **Japanese voices and movies** from an undub disc ([audio](psprecomp/games/dissidia/docs/audio.md)).
- **Cloud save sync** between your devices through a folder, WebDAV or Google Drive
  ([cloud sync](psprecomp/games/dissidia/docs/cloud-sync.md)).
- **Saves from any region**: a US, European or Japanese save works with any disc ([savedata](psprecomp/games/dissidia/docs/savedata.md)).
- **Online multiplayer** (experimental), below.
- **Controllers, keyboard and touch**: gamepads work out of the box; Android has touch controls with an editable layout.
- **Settings in the game**: from the title screen, the start menu or the battle pause menu.
- **New characters and moves**: the Move Studio and Blender add-on, below.
- **ISO patch mods**: community `.xdelta` patches for the disc install as mods you can switch off and on
  ([details](psprecomp/games/dissidia/docs/xdelta-mods.md)).

### Controls

Gamepads work out of the box. Keyboard: arrow keys to move, **X / Z / A / S** for the face buttons, **Q / W** for the shoulders,
**Space** = Start, **Tab** = Select, **Esc** = Settings (or **Guide** / **L3 + R3** on a pad).

## Multiplayer (experimental)

Communications mode works over the internet instead of PSP ad-hoc WiFi. The game connects to a party server, so there is no port to
forward: open Communications mode and everyone in the same room shows up in the lobby. The default room is public; set your own room
name under Settings > Network to play with friends only. A LAN mode is on the same page.

So far a full match has been played between two copies on one machine; more than two players, real LANs and long distances are untested,
and high latency may make a match unplayable. Everyone in a session runs at the lowest frame rate any player chose.
More: [multiplayer.md](psprecomp/games/dissidia/docs/multiplayer.md).

## Making characters

A character is one folder in `mods/`: its menu pictures, moves and models as files you can read and edit. Added characters only run in
this native build (on a PSP or in PPSSPP only changes to the game's own characters work).

1. **Start in the Move Studio.** Open `studio/index.html`, set the mods folder and your game ISO, then pick **+ New character…**. Give
   them a name, a series and the retail character they are built on (its skeleton, animations, voice and moves are the starting point).
2. **Make their moves**: copy moves from any character, place events on the timeline, tune each hit's damage and knock-back, edit the
   effects, and press **Save**. The **Character** tab sets the name, slot and menu pictures.
3. **Give them a body and animations** in Blender with the add-on: import the character from its folder, model or retarget, then
   **Save moves to folder**.
4. **Play.** Every folder in `mods/` is built in when the game starts, and the new character is on the Quick Battle grid. Delete the
   folder (or set `"enabled": false` in its `mod.json`) to take them out.
5. **Share** by zipping the folder. It holds data only, no scripts; anything borrowed from the retail game is read from the other
   player's own disc.

To change one of the game's own characters, pick them in the Studio, edit, and press **Save as mod**. Up to 6 characters can be added.

More: [mods/README.md](mods/README.md) (folder layout, command-line tools), [studio/README.md](studio/README.md) (the Move Studio),
[blender/README.md](blender/README.md) (the Blender add-on), [docs/](psprecomp/games/dissidia/docs/README.md) (roster, formats, events,
audio, how the build works).

## Building from source

Most people should use the installer above. To build from a checkout instead (Linux or macOS, about 8 GB of disk), put your game at
`~/dissidia/game.iso`, then:

```bash
git clone https://github.com/schindlershadow/dissidia ~/dissidia/repo
~/dissidia/repo/psprecomp/games/dissidia/tools/install.sh ~/dissidia/game.iso
```

It installs the build tools (with `apt` or Homebrew, plus Rust), extracts and decrypts the game and compiles it; if it stops part-way, run
it again. Play with `~/dissidia/work/play.sh`; update with the same command plus `--update`. Every step and runtime option:
[native-build.md](psprecomp/games/dissidia/docs/native-build.md). The installer and release pipeline:
[installer.md](psprecomp/docs/installer.md). Android builds: [android.md](psprecomp/docs/android.md).

## About

This started as a fork of [PSPRecomp](https://github.com/wizardengineer/psprecomp) (a static recompiler and runtime for PSP games) and has
grown into its own project; everything Dissidia-specific lives in `psprecomp/games/dissidia/`. It was developed with heavy use of an AI
assistant (Claude, by Anthropic), which did most of the reverse engineering and wrote most of the code, under my direction and testing.
Findings in the docs carry a confidence (confirmed / inferred / medium / low); inferred ones are educated guesses from disassembly.

Not in this repository: game data (bring your own disc), texture packs (downloaded by the setup from their authors' releases),
third-party mod ports and game artwork.

## Credits

- [PSPRecomp](https://github.com/wizardengineer/psprecomp): the recompiler and runtime this project grew from (GPL-2.0).
- [PPSSPP](https://github.com/hrydgard/ppsspp): VFPU maths (`psprecomp/runtime/src/vfpu_ppsspp`, GPL-2.0+), and the emulator used for every comparison and trace.
- [DissDlcToolkit](https://github.com/adriangl/DissDlcToolkit) (adriangl): the object-table format and the DLC route.
- [dissidia-012-sora](https://github.com/MatNyaga/dissidia-012-sora) (MatNyaga): notes on effect-package ids and CPU AI.
- [D012FFRT](https://github.com/Niosa/D012FFRT) (Niosa): the community HD texture pack under our upscales.
- [atracdenc](https://github.com/dcherednik/atracdenc): ATRAC3 encoding for new BGM.
- Fonts: Cinzel and Crimson Text (SIL Open Font License).
- Everyone in the Dissidia modding community without whom this work could never have happened.

## Licence

GPL-2.0 (see `psprecomp/LICENSE`). Dissidia 012 Duodecim and Final Fantasy are trademarks of Square Enix; this project is not affiliated
with or endorsed by Square Enix.
