# Savage Wind Recomp

> [!IMPORTANT]
> **This project is made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation here is written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by
> a human. It builds on [LAS Recomp](https://github.com/peiikai/LASRecomp), which was made the same way. If you don't
> trust code written by AI, you can simply ignore this project. As with anything on the internet, you run the code at
> your own discretion.

**Savage Wind Recomp** is a work-in-progress static recompilation of *The Legend of Zelda: Breath of the Wild*
(Nintendo Switch, version 1.9.0) into a native application for Windows and Linux. The goal is a full PC
version of your own copy of the game: a launcher, PC-style graphics options, mods, any controller, keyboard and
mouse, and its text and voices in any of its languages.

> [!WARNING]
> **Playable, but how far is unknown.** Savage Wind plays the opening in the Shrine of Resurrection and the Great
> Plateau around it at 60 fps, with sound, voices, the HUD, saving and loading. Nobody has played past the Great
> Plateau on Savage Wind yet, so expect bugs or crashes further on. This is an alpha, a preview for testers. If you
> find a bug, please [report it on GitHub](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose): every
> report shows how far the game really works.

> [!TIP]
> **Want to contribute? Look at the [`test`](https://github.com/Revivedx/SavageWindRecomp/tree/test) branch first.**
> It has every change that is implemented and being tested, ahead of the latest release, so you don't start on
> something that's already done or in progress. Base your work on `test` and open pull requests against it: `main`
> only moves for a release. See [CONTRIBUTING.md](CONTRIBUTING.md).

> [!NOTE]
> **No game files are included.** Savage Wind Recomp needs your own legally obtained copy of the game, updated to
> **version 1.9.0**, dumped from **your own** Nintendo Switch. This project does not include, and will not
> provide, game files, keys, firmware, dumping or decryption tools, or instructions on how to obtain any of them.
> Savage Wind never decrypts anything: it only reads files you have already extracted yourself. The recompilation of
> the game code happens locally, on your machine, from your files.

## Before you report a bug

- **Use the [latest release](https://github.com/Revivedx/SavageWindRecomp/releases/latest)** and game version
  **1.9.0**: 1.8.2 isn't supported, and many bugs of older releases are already fixed.
- **Search the [existing issues](https://github.com/Revivedx/SavageWindRecomp/issues?q=is%3Aissue)** first.
- **Attach `savagewind.log`.** On Windows it's next to `savagewind.exe`, in the Savage Wind folder; on Linux, in
  `~/.local/share/savagewind/`. The game writes a new one each time it starts, so take it right after the bug.
- Reports missing any of these get a `needs-info` label and are closed after 7 days without it.

## Getting started (Alpha 3)

**You need:** a 64-bit CPU with SSE4.2 (AVX2 runs faster), a GPU with a Vulkan 1.3 driver, and 16 GB of RAM. With
integrated graphics, which share the PC's RAM, 8 GB isn't enough: the game's 3 GB of memory has to stay available
to the GPU ([#117](https://github.com/Revivedx/SavageWindRecomp/issues/117)).

1. Download from [Releases](https://github.com/Revivedx/SavageWindRecomp/releases):
   - **Windows:** `SavageWind-0.3.1-alpha.3-windows.zip`. Extract it to a folder you can write to (not Program
     Files): your game files, saves and settings are kept in that folder. 0.3.1 is a hotfix for AMD and Intel
     graphics on Windows (a black window in 0.3.0, [#85](https://github.com/Revivedx/SavageWindRecomp/issues/85)); on
     NVIDIA it runs exactly as 0.3.0.
   - **Linux (x86-64, Steam Deck too):** `Savage_Wind-0.3.0-alpha.3-x86_64.AppImage`. Make it executable
     (`chmod +x`) and run it; your game files, saves and settings go to `~/.local/share/savagewind`. See
     [docs/linux.md](docs/linux.md).
   - **Linux on ARM (aarch64):** `Savage_Wind-0.3.0-alpha.3-aarch64.AppImage` is **highly experimental**: it has
     never run on real ARM hardware. Reports from real hardware are welcome ([docs/linux.md](docs/linux.md),
     "Testing on ARM").
2. Run `savagewind.exe` (or the AppImage). The first time it asks for your game files: pick the folder with your own
   extracted game (`exefs/` and `romfs/`, version 1.9.0, see below) and choose **Set up game files**. It checks the
   version, copies the files and recompiles the game code on your machine - once (about 6
   minutes for the game code with a 12-thread CPU, plus the copy and the optional shader precompilation). Your own
   files aren't changed. When it's done the game starts. For the DLC (from the next release), see
   [The DLC](#the-dlc).
3. From then on Savage Wind goes straight into the game. The settings, including the text and voice languages,
   are in the game's Options (pause menu > System > Options, or Options on the title screen).
4. **Savage Wind Configuration** (`Savage Wind Configuration.exe` on Windows, the AppImage's **Savage Wind
   Configuration** desktop action or `--config` on Linux) opens the configuration screen: Play, Settings, your game
   files (set them up again, rebuild the game code, delete them) and Quit. In game, Quit to Start Screen also comes
   back to it.

Coming from Alpha 2: extract Alpha 3 to a new folder and set the game files up again with your 1.9.0 files (the game
code has to be built again); copy your `save` folder over to keep your progress. What's new is in
[CHANGELOG.md](CHANGELOG.md).

To keep game files, saves and caches on another drive, see [Moving the data folder](docs/data-folder.md).

The setup and configuration screens show Savage Wind's own art. To see a picture of your own there instead, put it
next to `savagewind.exe` (on Linux, in the data folder) as `splash.jpg` or `splash.png`; it's only read from your folder and never part of
Savage Wind.

Needs Windows 10 or later, a CPU with SSE4.2 (2008 or newer), a GPU with Vulkan 1.3 and about 20 GB of free disk
space. AVX2 isn't required: on a CPU without AVX2 / FMA the setup builds the game code in a compatible mode, which runs
slower.

## Where are my saves?

Saves are in the `save` folder inside Savage Wind's data folder:

| Platform | Default save location |
| --- | --- |
| Windows | `save\` next to `savagewind.exe` |
| Linux / Steam Deck | `~/.local/share/savagewind/save/`, or `$XDG_DATA_HOME/savagewind/save/` if `XDG_DATA_HOME` is set to an absolute path |

If you set `SAVAGEWIND_DATA_ROOT`, look for `save` inside that folder instead. Source builds started from a folder
containing `extracted/` use that starting folder as the data folder. The startup log's `[paths] data: ...` line shows
the data folder actually in use. `SAVAGEWIND_SAVE_DIR`, when set for a test run, overrides the save folder separately.

To back up your progress or move it to another Savage Wind installation, close the game and copy the **whole
`save` folder**, including `save_0` and its contents. Keep a backup of the destination's saves before replacing them.

Played on the Wii U or in Cemu? Savage Wind Configuration can import that save: open **Settings > Saves** and choose
**Import from Wii U / Cemu...**. It backs up your current saves first and asks before it replaces them. See
[Importing Wii U / Cemu saves](docs/save-converter.md).

## What you need

- Your own *Breath of the Wild* (cartridge or digital) and its **1.9.0 update**, the latest, extracted from your own
  console:
  - the **ExeFS** of that version (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`)
  - the **RomFS** with that update applied (base game and update merged)
- Optional, from the next release (Alpha 4; on the `test` branch now): the DLC, *The Master Trials* and *The
  Champions' Ballad*, from your own copy: the **RomFS** of each of the two DLC, version 3.0 (see
  [The DLC](#the-dlc) below).
- Windows 10 or later or Linux, on x86-64 (a CPU with SSE4.2; without AVX2 / FMA the game code runs in a slower
  compatible mode), and a GPU with Vulkan 1.3.

Keep them in one folder like this one. The setup asks for it, and its name and place don't matter; a source build
reads `extracted/` next to the program (git ignores this folder):

```
extracted/
  exefs/   main, main.npdm, rtld, sdk, subsdk0
  romfs/   the complete RomFS (base game + the 1.9.0 update merged)
  dlc1/    optional: the first DLC's RomFS (11 folders, about 7,000 files, 1.4 GB)
    Game/  Map/  Movie/  NavMesh/  Pack/  Physics/  Sound/  System/  Terrain/  UI/  Voice/
  dlc2/    optional: the second DLC's RomFS (1 folder, 1 file)
    System/AocVersion.txt
```

The `main` module's build id tells the exact version; `python tools/sw_probe.py` (it reads `extracted/exefs`)
prints it and what the recompiler will find there.

### The DLC

Each DLC's RomFS goes in a folder of its own, like `dlc1/` and `dlc2/` above: its folders right inside it
(`dlc1/System/AocVersion.txt`) or one folder deeper (`dlc1/romfs/System/AocVersion.txt`); the setup finds either.

- **With the game:** keep them in the same folder as `exefs/` and `romfs/`, and choose that folder at the first
  setup. The DLC is set up together with the game.
- **Later:** in Savage Wind Configuration's game files page, choose a folder that holds just the DLC folders and set
  up. The DLC is added to the game files already set up, without building the game code again.

The setup tells the game's RomFS (it has `System/Version.txt`) from a DLC's (`System/AocVersion.txt`), so the folder
names don't matter. It copies them as `aoc1/` and `aoc2/` into the data folder. The game files page shows the DLC set
up (for example "1 + 2 (3.0)") and removes it; your saves stay.

The DLC are optional: the whole game plays without them. What you get depends on what you set up:

| Set up | What you get |
|---|---|
| No DLC | The whole game, as on a console without the DLC |
| Only the first or only the second DLC | The whole game, without DLC content: the game uses the DLC only when it finds both |
| Both DLC | The whole game plus Master Mode, the Trial of the Sword, the Travel Medallion and The Champions' Ballad; the title screen shows "DLC Ver.3.0" |

The two packs came out months apart in 2017, both part of the Expansion Pass. In the version the game has now (DLC
3.0), the first DLC holds the data of both packs, *The Champions' Ballad* included, and the second only tells the
game that you have the second pack. So setting up only one of them never breaks your game: it runs as without DLC.
To unlock the DLC content, set up both, from your own console and your own Expansion Pass.

## Status

Estimates of where each part stands. "Tested in game" is how much of the game has been played through on Savage Wind so far.

| | Part | Progress | What it means |
|---|---|---|---|
| ✅ | **Recompiler** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The whole game (main, sdk and multimedia modules) recompiles to native code |
| ✅ | **Game files setup** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Version check, copy, data image and code build, all from your own files |
| ✅ | **Text and voice languages** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Any of the game's 15 text languages (16 with 1.9.0's Thai) with any of its 8 voice languages |
| ✅ | **Voices in another language: crash fix** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Fixed ([#2](https://github.com/Revivedx/SavageWindRecomp/issues/2)): the game now sets aside enough memory for voice banks bigger than its own, so any voice language works with any text language |
| ✅ | **Menus in 13 languages** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Savage Wind's own screens follow the text language. Hebrew, right to left, which the game has no text language for: `SAVAGEWIND_MENU_LANG=he` |
| ✅ | **Sound and voices** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Mixed by the game SDK's own audio renderer, with room reverb |
| ✅ | **Keyboard and mouse** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Every key and mouse button remappable, mouse camera |
| ✅ | **Settings in the game's Options** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video, audio, controls and language pages drawn by the game itself |
| ✅ | **Direct start + Configuration** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | savagewind.exe goes straight into the game; Savage Wind Configuration has the rest |
| ✅ | **Window sizes and fullscreen** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The sizes your display can show; windowed, borderless or exclusive |
| ✅ | **Ultrawide** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video > Fill wide screens (on by default): a window or screen wider than 16:9 (21:9, 32:9) shows more of the world to the sides instead of bars, and a taller one (16:10, 4:3) more above and below. The HUD stays 16:9 in the middle; the pause menu, the map, the loading screen and the start screen are shown 16:9 |
| ✅ | **Internal resolution and FSR 1** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Native resolution up to 4K, or AMD FSR 1 quality modes |
| ✅ | **Anisotropic filtering and sharpening** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Ground and paths sharp at a distance (2x to 16x); AMD CAS sharpening of the 3D image, useful with FSR 1 |
| ✅ | **Higher-resolution shadows** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Sun shadows at the console's 896x896, or sharper at 2048, 4096 or 8192 (Video > Shadow resolution) |
| ✅ | **Shader and pipeline cache** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Places you've already seen don't stutter again |
| ✅ | **Full shader precompilation** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Optional and on by default, at setup or later in Savage Wind Configuration: all of the game's ~14,000 shaders are translated and compiled ahead of time, so new places and effects don't stutter the first time |
| ✅ | **Faster game code build** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The one-time setup takes about 15 minutes instead of about 40 (12 threads) from the next release: the game code builds in about 6 minutes instead of 30 ([#43](https://github.com/Revivedx/SavageWindRecomp/pull/43), by [@adriadam10](https://github.com/adriadam10)) |
| ✅ | **Frame rate** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | 60 fps by default, or 30, 90, 120 or no limit, with the game at the same speed; menus, cutscenes and conversations too, and videos at their own speed |
| ✅ | **Field of view** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video > Field of view: 70% to 150% of the game's own in play; cutscenes and conversations keep theirs |
| ✅ | **Draw distance** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video > Draw distance: Original (the game's own), Medium or High. Trees, rocks, buildings, enemies and villagers appear further out (1.5x at Medium, 1.6x at High, up to 2 km), buildings keep their full models further before their far versions take over, and distant landmarks such as Hyrule Castle and the terrain's distant trees stay drawn 1.5x as far. The grass reaches further too: the short grass 1.6x (Medium) or 2.5x (High), the tall grass about 1.15x or 2.45x. The terrain's own detail levels stay the game's ([#88](https://github.com/Revivedx/SavageWindRecomp/issues/88)) |
| ✅ | **Steady 60 fps in the open world** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Holds 60 fps in real play on the Great Plateau (RTX 3060, Ryzen 5 5600X); steady 120 fps (8 ms frames) is the next goal |
| ✅ | **Game version 1.9.0** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The latest version and the supported one (its Thai text isn't offered yet). 1.8.2 still sets up and runs, but isn't supported: a bug is only fixed if it also happens on 1.9.0 |
| ✅ | **DLC** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | *The Master Trials* and *The Champions' Ballad* from your own copy, set up with the game files or later ([The DLC](#the-dlc)), from the next release: Master Mode, the Trial of the Sword and the Travel Medallion work on a 100% save. The Champions' Ballad has been played through. |
| ✅ | **Variable refresh rate (VRR)** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video > VSync: On lets G-SYNC and FreeSync displays show every frame as soon as it's ready, a little under the refresh rate; Fast (the default), Adaptive and Off too. Tried on a VRR display |
| ✅ | **Motion controls (gyro)** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The controller's gyroscope and accelerometer (DualSense, DualShock 4, Switch controllers, Steam Deck) for motion aiming and the tilt shrines; without one, hold T and move the mouse to tilt. Sensitivity and inversion in Options > Controls. Confirmed by players with real controllers |
| 🔧 | **Horizon OS services** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | Threads, files, controllers, applets and time: all the opening and the Great Plateau use |
| 🔧 | **Maxwell shader translator** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | Every shader seen so far translates to Vulkan |
| 🔧 | **Graphics (NVN on Vulkan)** | ![80%](https://progress-bar.xyz/80/?width=150&style=square&progress_color=5cb85c) | Shrine and Great Plateau look right, shadows included; far parts of the map can still be missing ([#20](https://github.com/Revivedx/SavageWindRecomp/issues/20)) |
| 🔧 | **Controllers** | ![80%](https://progress-bar.xyz/80/?width=150&style=square&progress_color=5cb85c) | Any controller SDL knows, by button position, confirmed by players; automatic recognition is next, then camera speed set apart for each axis |
| 🔧 | **Button icons for your device** | ![80%](https://progress-bar.xyz/80/?width=150&style=square&progress_color=5cb85c) | The game's button prompts show your bound keys and mouse buttons, or Xbox / PlayStation / Nintendo buttons, switching with the device you use |
| 🔧 | **Tested in game** | ![50%](https://progress-bar.xyz/50/?width=150&style=square&progress_color=f0ad4e) | The Shrine of Resurrection, the Great Plateau, and a walk through every region on a 100% save file. The main quests and the Divine Beasts aren't tested yet, so expect bugs there: [report what breaks](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose) |
| 🔧 | **Linux** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | Builds and runs on x86-64 (from LAS Recomp); a steady 144 fps on an RTX 3070 Ti with a Ryzen 5 7600X, and no Linux-only issues so far |
| 🔧 | **AMD FSR 3 + frame generation** | ![60%](https://progress-bar.xyz/60/?width=150&style=square&progress_color=f0ad4e) | Video > Frame generation (AMD FSR 3): a frame made between each two the game draws, about twice the frames on screen, with the HUD kept sharp. Motion vectors from depth and the camera, and Link's own; enemies and animals can still leave a faint smear. Smooth with VSync and G-SYNC. Works on Windows; Linux and the Steam Deck aren't tried yet. Only frame generation for now: FSR 3 upscaling is next |
| 🗺️ | **NVIDIA DLSS (RTX)** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned, after FSR 3; under NVIDIA's own SDK license |
| 🔧 | **Colour grading and HDR** | ![50%](https://progress-bar.xyz/50/?width=150&style=square&progress_color=f0ad4e) | Options > Colour: presets, exposure, contrast, saturation, temperature, tint and LUTs (three CC0 ones included; your own .cube files and ReShade .png strips go in the `luts` folder), on the 3D image only: the HUD keeps its colours. Windows and Linux. Planned: fog and bloom on or off, and real HDR output on HDR displays |
| 🗺️ | **Draw distance and detail** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: trees, grass and objects drawn farther away on strong PCs, or nearer for more fps, and a fixed level of detail (the most detailed models at any distance) |
| 🗺️ | **Free camera and photo mode** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: a free camera with the HUD hidden and its own speed and sensitivity, for screenshots, and camera paths through keyframes for videos |
| 🗺️ | **Effects on or off** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: a Video page that turns each of the game's own effects on or off, where BOTW has them: ambient occlusion, volumetric fog, grass (3 layers), character light (normal and custom), tone mapper, fragment and main lighting, bloom (3 layers), lens flare, cel shading, fog effects, water, torch fire, the sword sheath, depth of field (also when aiming the bow), screen-space reflections, global illumination and far lights |
| 🗺️ | **Graphics extras** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Later: ambient occlusion, volumetric fog and depth of field |
| 🗺️ | **Built-in benchmark** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: fixed camera routes that measure fps and frame times and save the results, to compare settings |
| 🗺️ | **GPU readings on AMD and Intel (Windows)** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: the performance overlay's GPU use and VRAM for any GPU on Windows ([#58](https://github.com/Revivedx/SavageWindRecomp/issues/58)); today only NVIDIA, and AMD on Linux |
| 🗺️ | **Mods** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned |
| 🗺️ | **Accessibility and assists** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: text size, colour-blind filters and toggles in place of held buttons; enemy and Link health, damage dealt and taken, weapon durability and stamina use on screen; optional assists: infinite stamina, health or weapon durability, arrows and rupees, no damage at all, no fall damage, health that refills over time, nearby enemies defeated at once, and multipliers for weapon durability, stamina use, damage dealt and taken, and walking, running, climbing, swimming and gliding speed |
| 🗺️ | **Teleport with bookmarks** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: save places and travel back to them |
| 🗺️ | **Achievements** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: achievements from your save's progress, from start to finish ([#236](https://github.com/Revivedx/SavageWindRecomp/pull/236)) |
| 🗺️ | **amiibo rewards without figures** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned |

✅ done · 🔧 in progress · 🗺️ roadmap

## Controls

Any controller SDL knows works, by button position; one with a gyroscope (DualSense, DualShock 4, Switch controllers,
Steam Deck) also aims and tilts by motion.

Controller buttons, sticks and motion only control the game while its window has focus. Switching away releases
held inputs; returning reads the controller's current state.

Keyboard and mouse defaults:

| Action | Key | Action | Key |
|---|---|---|---|
| Move | W A S D | Camera | Mouse (or I J K L) |
| Walk (hold) | C | Crouch | Left Ctrl |
| Jump (X) | Space | Sprint / back (B) | Left Shift |
| Interact / confirm (A) | E | Attack (Y) | Left click |
| Shield / lock on (ZL) | Right click | Bow (ZR) | F |
| Rune (L) | Q | Throw (R) | R |
| Look (right stick press) | Middle click | Weapons / shields / runes / whistle (d-pad) | 1 / 2 / 3 / 4 |
| Inventory (Plus) | Tab | Map (Minus) | M |
| Tilt the controller (hold, motion puzzles) | T + mouse | | |

Every key and mouse button can be remapped in the game's Options (the Keyboard pages), next to the mouse sensitivity
(horizontal and vertical) and Y inversion, motion sensitivity and inversion and the tilt key (Controls). The game's
button prompts show the keys you bound, or your controller's own buttons (Controls > Button icons: Automatic follows
the device you use). The mouse turns the camera directly, as in a PC game: 0.022° per count at 100% sensitivity (as
Quake and Source count it), on both axes (the vertical one scaled by Controls > Mouse vertical sensitivity), with no
acceleration or top speed, the same at any frame rate. The bow's and the scope's own slower speeds stay, and so do the
game's camera speed setting and its camera axis inversion options. Esc frees the cursor, a click in the window takes
it back. F10 cycles the performance overlay (off, FPS, full). Full also shows Link's XYZ coordinates from the game's
position copy, in game units, with one decimal place. The line is hidden on the title screen, while loading,
during movies and scripted events (cutscenes, talks, item pickups), or when the position is unavailable.

## Building from source

### Windows

More detail in [docs/BUILD-windows.md](docs/BUILD-windows.md).

1. One-time setup, in PowerShell from the repository root:
   `powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1`.
   It installs MSYS2 (UCRT64: clang, make, SDL3, FFmpeg, Vulkan, shaderc) and Python for the tools, and switches
   the clone to LF line endings.
2. From an MSYS2 UCRT64 shell (or Git Bash with `export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"`):
   - `make -j` builds the app, `build/savagewind.exe`.
   - Put your files in `extracted/` (above), then `make gen` (recompiles the game code into `generated/`, about a
     minute) and `make -j` again (builds it into `build/swgame.dll`; the first build takes 15-20 minutes).
3. Run: `build/savagewind.exe extracted --play`, from a shell with `C:\msys64\ucrt64\bin` on PATH.

### Linux

See [docs/linux.md](docs/linux.md): the same steps with the distribution's clang, SDL 3, FFmpeg, shaderc and Vulkan
Memory Allocator (`build/savagewind`, `build/swgame.so`).

Developer notes (in Spanish) - what was changed from LAS Recomp, how the boot got this far, and the debugging
switches - are in [docs/SAVAGE-WIND.md](docs/SAVAGE-WIND.md).

## Respect copyright

- *The Legend of Zelda: Breath of the Wild* is © Nintendo. This is a fan project. It is not affiliated with,
  endorsed by or sponsored by Nintendo. *Nintendo Switch* and *The Legend of Zelda* are trademarks of Nintendo.
- This repository contains no game files, no game code (also not the C code the recompiler generates from it), no
  assets, no fonts, no keys - and release builds won't either.
- The game code you generate and the build made from it contain Nintendo's code. They are for your own use only:
  don't share them, upload them or ship them.
- Issues, pull requests or discussions that contain game files, keys, generated code or links to pirated copies
  will be removed. Piracy is not supported in any way.
- Follow the copyright laws of your country. Dumping a game you own for personal use is legal in some countries and
  not in others; that is your responsibility.

## Credits

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - the recompiler, the runtime and most of what
  this project is built on (MIT).
- [Ryujinx](https://github.com/Ryujinx/Ryujinx) (MIT) - its shader decoder was the reference for instruction field
  layouts.
- [envytools](https://github.com/envytools/envytools) - NVIDIA GPU documentation used for the shader translator.
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org)).
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and the
  [Switch Loader](https://github.com/Adubbz/Ghidra-Switch-Loader) - used for reverse engineering (not bundled).
- Libraries: [SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org) (LGPL),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT),
  [SMAA](https://github.com/iryoku/smaa) (MIT), [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (MIT), [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT),
  [AMD FidelityFX SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4) (MIT: FSR 3 frame
  generation),
  [Fira Sans](https://github.com/mozilla/Fira) (SIL OFL 1.1), [Noto Sans Thai](https://github.com/notofonts/thai)
  (SIL OFL 1.1), [Noto Sans Hebrew](https://github.com/notofonts/hebrew) (SIL OFL 1.1), [stb_image / stb_image_write](https://github.com/nothings/stb)
  (public domain / MIT), [shaderc](https://github.com/google/shaderc)
  (Apache 2.0), [Zig](https://ziglang.org) (MIT), [Capstone](https://www.capstone-engine.org) (BSD),
  [Unicorn](https://www.unicorn-engine.org) (GPL 2.0), [python-lz4](https://github.com/python-lz4/python-lz4) (BSD).
- Colour LUTs from [FreshLUTs](https://freshluts.com), CC0 1.0, renamed: "Savage Wind" is
  [CINEMA 3](https://freshluts.com/luts/219) and "Dormant" is [CARIBE](https://freshluts.com/luts/217), both
  uploaded by Andy; "Ember" is [powerful cold](https://freshluts.com/luts/107), uploaded by sagi hershkovich
  (`third_party/luts/README.md`).

## License

Savage Wind Recomp's own code is open source under the [MIT License](LICENSE), as is LAS Recomp's, on which it
builds. Third-party components in `third_party/` keep their own licenses. The license covers this project's code
only: it grants nothing for Nintendo's game, which you must supply yourself.

The Windows release zip also bundles, unchanged unless noted, under their own licenses:

- FFmpeg 9.0.2 (libavcodec, libavformat, libavutil, libswscale), LGPL 2.1 or later: built from the official
  [source](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz) as LGPL only, without GPL or nonfree parts, by
  [tools/build_deps_windows.sh](tools/build_deps_windows.sh). They are separate DLLs: you can replace them with your
  own build of the same version.
- From [MSYS2](https://packages.msys2.org) (UCRT64, sources there): SDL 3.4.18 (zlib), shaderc 2026.4 (Apache 2.0),
  libiconv 1.19 (LGPL 2.1 or later), the GCC 16.2 runtime (libgcc, libstdc++: GPL 3 with the GCC Runtime Library
  Exception) and winpthreads (MIT and BSD).
- Zig 0.17.0 (MIT; its bundled LLVM, Clang and LLD: Apache 2.0 with LLVM Exceptions; the MinGW-w64 headers and
  runtime: their own permissive licenses), trimmed to what the game code build uses.
- `swgen/`, this project's generator frozen with PyInstaller 6.22 (GPL 2.0 with the bootloader exception): Python
  3.14 (PSF License), Capstone 5.0.9 (BSD), [Unicorn 2.1.4](https://github.com/unicorn-engine/unicorn/tree/2.1.4)
  (GPL 2.0, source at that link) and python-lz4 4.4.5 (BSD).
- AMD FidelityFX SDK 1.1.4 (MIT, `FidelityFX-LICENSE.txt`), compiled into savagewind.exe: FSR 3 frame interpolation,
  optical flow and the Vulkan backend, from [its source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4)
  with two alignment fixes (`third_party/FidelityFX/README.md`) and shaders compiled by `tools/ffx_shaders.py`.
- The Fira Sans font, Medium and SemiBold (SIL OFL 1.1, `FiraSans-OFL.txt`), and the Noto Sans Thai and Noto Sans
  Hebrew fonts, Medium, for the menus in Thai and Hebrew (SIL OFL 1.1, `NotoSansThai-OFL.txt`,
  `NotoSansHebrew-OFL.txt`).
- Three colour LUTs in `colour-luts/` (CC0 1.0, from FreshLUTs: see "Credits" and `colour-luts/README.md`).
