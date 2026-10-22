# Savage Wind Recomp

> [!IMPORTANT]
> **This project is made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation here is written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by
> a human. It builds on [LAS Recomp](https://github.com/peiikai/LASRecomp), which was made the same way. If you don't
> trust code written by AI, you can simply ignore this project. As with anything on the internet, you run the code at
> your own discretion.

**Savage Wind Recomp** is a work-in-progress static recompilation of *The Legend of Zelda: Breath of the Wild*
(Nintendo Switch, version 1.8.2) into a native application for Windows and Linux. The goal is a full PC version of
your own copy of the game: a launcher, PC-style graphics options, mods, any controller, keyboard and mouse, and
its text and voices in any of its languages.

> [!WARNING]
> **Playable, but how far is unknown.** Savage Wind plays the opening in the Shrine of Resurrection and the Great
> Plateau around it at 60 fps, with sound, voices, the HUD, saving and loading. Nobody has played past the Great
> Plateau on Savage Wind yet, so expect bugs or crashes further on. This is an alpha, a preview for testers. If you
> find a bug, please [report it on GitHub](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose): every
> report shows how far the game really works.

> [!NOTE]
> **No game files are included.** Savage Wind Recomp needs your own legally obtained copy of the game, updated to
> **version 1.8.2**, dumped from **your own** Nintendo Switch. This project does not include, and will not provide,
> game files, keys, firmware, dumping or decryption tools, or instructions on how to obtain any of them. Savage Wind
> never decrypts anything: it only reads files you have already extracted yourself. The recompilation of the game
> code happens locally, on your machine, from your files.

## Getting started (Alpha 2, Windows)

1. Download `SavageWind-0.2.0-alpha.2-windows.zip` from [Releases](https://github.com/Revivedx/SavageWindRecomp/releases)
   and extract it to a folder you can write to (not Program Files): your game files, saves and settings are kept in
   that folder.
2. Run `savagewind.exe`. The first time it asks for your game files: pick the folder with your own extracted game
   (`exefs/` and `romfs/`, version 1.8.2, see below) and choose **Set up game files**. It checks the version, copies
   the files, builds the data image and recompiles the game code on your machine - once, and it takes a while (about
   40 minutes with a 12-thread CPU). Your own files aren't changed. When it's done the game starts.
3. From then on `savagewind.exe` goes straight into the game. The settings, including the text and voice languages,
   are in the game's Options (pause menu > System > Options, or Options on the title screen).
4. **Savage Wind Configuration** (`Savage Wind Configuration.exe`, in the same folder) opens the configuration
   screen: Play, Settings, your game files (set them up again, rebuild the game code, delete them) and Quit. In
   game, Quit to Start Screen also comes back to it. (Built from source on Linux or macOS: `--config`, the
   AppImage's **Savage Wind Configuration** desktop action, or Option held while opening the Mac app.)

The setup and configuration screens show Savage Wind's own art. To see a picture of your own there instead, put it
next to `savagewind.exe` as `splash.jpg` or `splash.png`; it's only read from your folder and never part of
Savage Wind.

Needs Windows 10 or later, a CPU with AVX2, a GPU with Vulkan 1.3 and about 35 GB of free disk space.

## What you need

- Your own *Breath of the Wild* (cartridge or digital) and its **1.8.2 update**, extracted from your own console:
  - the **ExeFS** of 1.8.2 (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`)
  - the **RomFS** with the update applied (base game and 1.8.2 merged)
- DLC: not supported yet (planned, see the roadmap).
- Windows 10 or later (x86-64, a CPU with AVX2) or Linux (x86-64), and a GPU with Vulkan 1.3.

The files go in `extracted/` next to the program (git ignores this folder):

```
extracted/
  exefs/   main, main.npdm, rtld, sdk, subsdk0
  romfs/   the complete RomFS (base game + 1.8.2 merged)
```

The `main` module's build id tells the exact version; `python tools/sw_probe.py` (it reads `extracted/exefs`)
prints it and what the recompiler will find there.

## Status

Estimates of where each part stands. "Tested in game" is how much of the game has been played through on Savage Wind so far.

| | Part | Progress | What it means |
|---|---|---|---|
| ✅ | **Recompiler** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The whole game (main, sdk and multimedia modules) recompiles to native code |
| ✅ | **Game files setup** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Version check, copy, data image and code build, all from your own files |
| ✅ | **Latest game version** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Version 1.8.2, the latest |
| ✅ | **Text and voice languages** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Any of the game's 15 text languages with any of its 8 voice languages |
| ✅ | **Menus in 11 languages** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Savage Wind's own screens follow the text language |
| ✅ | **Sound and voices** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Mixed by the game SDK's own audio renderer, with room reverb |
| ✅ | **Keyboard and mouse** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Every key and mouse button remappable, mouse camera |
| ✅ | **Settings in the game's Options** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Video, audio, controls and language pages drawn by the game itself |
| ✅ | **Direct start + Configuration** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | savagewind.exe goes straight into the game; Savage Wind Configuration has the rest |
| ✅ | **Window sizes and fullscreen** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | The sizes your display can show; windowed, borderless or exclusive |
| ✅ | **Internal resolution and FSR 1** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Native resolution up to 4K, or AMD FSR 1 quality modes |
| 🔧 | **Motion vectors, TAA and AMD FSR 3.1** | ![80%](https://progress-bar.xyz/80/?width=150&style=square&progress_color=5cb85c) | The camera's and each object's own motion; TAA, and FSR 3.1 from Native AA to Ultra Performance (F12 menu, `amd_fidelityfx_vk.dll` next to the game) |
| 🔧 | **Frame generation** | ![50%](https://progress-bar.xyz/50/?width=150&style=square&progress_color=f0ad4e) | 2x / 3x / 4x: the in-between images made from the game's last two frames along the motion vectors and an optical flow, the HUD kept still (F12 menu); almost free |
| ✅ | **Shader and pipeline cache** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | Places you've already seen don't stutter again |
| ✅ | **Frame rate** | ![100%](https://progress-bar.xyz/100/?width=150&style=square&progress_color=5cb85c) | 60 fps by default, or 30, 90, 120 or no limit, with the game at the same speed; menus, cutscenes and conversations at 30, as on the console |
| 🔧 | **Horizon OS services** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | Threads, files, controllers, applets and time: all the opening and the Great Plateau use |
| 🔧 | **Maxwell shader translator** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | Every shader seen so far translates to Vulkan |
| 🔧 | **Graphics (NVN on Vulkan)** | ![75%](https://progress-bar.xyz/75/?width=150&style=square&progress_color=5cb85c) | Shrine and Great Plateau look right, shadows included; two save-screen glitches and a black pause menu background after an in-game restart left |
| 🔧 | **Controllers** | ![70%](https://progress-bar.xyz/70/?width=150&style=square&progress_color=5cb85c) | Any controller SDL knows, by button position; automatic recognition is next |
| 🔧 | **Steady 60 fps in the open world** | ![90%](https://progress-bar.xyz/90/?width=150&style=square&progress_color=5cb85c) | 60 fps in a scripted walk at 2560x1440 (RTX 3060, Ryzen 5 5600X); real play still drops to 45-50 fps in places, and the first time an effect appears it stutters while its pipeline is made |
| 🔧 | **Tested in game** | ![10%](https://progress-bar.xyz/10/?width=150&style=square&progress_color=d9534f) | The Shrine of Resurrection and the Great Plateau; the rest of Hyrule is untested: [report what breaks](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose) |
| 🗺️ | **Linux** | ![30%](https://progress-bar.xyz/30/?width=150&style=square&progress_color=f0ad4e) | Builds and runs on x86-64 (from LAS Recomp); not yet tested with this game |
| 🗺️ | **NVIDIA DLSS (RTX)** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned, after FSR 3; under NVIDIA's own SDK license |
| 🗺️ | **Variable refresh rate (VRR)** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: on G-SYNC and FreeSync displays every frame shows as soon as it's ready, without tearing or vsync stutter |
| 🗺️ | **Faster game code build** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: the one-time setup build in about 20 minutes instead of about 40 (12 threads) |
| 🗺️ | **Full shader precompilation** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Optional, at setup or in Configuration: no first-time stutter anywhere |
| 🗺️ | **Mods** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned |
| 🗺️ | **Accessibility options** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: text size, colour-blind filters and toggles in place of held buttons; enemy and Link health, damage dealt and damage taken |
| 🗺️ | **Button icons for your device** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned: the game's button prompts show your keyboard and mouse keys, or the buttons of the controller you're using |
| 🗺️ | **DLC** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Later |
| 🗺️ | **amiibo rewards without figures** | ![0%](https://progress-bar.xyz/0/?width=150&style=square&progress_color=d9534f) | Planned |

✅ done · 🔧 in progress · 🗺️ roadmap

## Controls

Any controller SDL knows works, by button position. Keyboard and mouse defaults:

| Action | Key | Action | Key |
|---|---|---|---|
| Move | W A S D | Camera | Mouse (or I J K L) |
| Walk (hold) | C | Crouch | Left Ctrl |
| Jump (X) | Space | Sprint / back (B) | Left Shift |
| Interact / confirm (A) | E | Attack (Y) | Left click |
| Shield / lock on (ZL) | Right click | Bow (ZR) | F |
| Rune (L) | Q | Throw (R) | R |
| Look (right stick press) | Middle click | D-pad | Arrow keys |
| Inventory (Plus) | Tab | Map (Minus) | M |

Every key and mouse button can be remapped in the game's Options (the Keyboard pages), next to the mouse sensitivity
and Y inversion (Controls). Esc frees the cursor, a click in the window takes it back. F10 cycles the performance
overlay (off, FPS, full).

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
   - `build/savagewind.exe extracted --build-romfs` packs your RomFS into the image the runtime reads
     (`extracted/romfs.bin`, about 15 GB).
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
  (MIT), [Fira Sans](https://github.com/mozilla/Fira) (SIL OFL 1.1), [stb_image](https://github.com/nothings/stb)
  (public domain / MIT), [shaderc](https://github.com/google/shaderc)
  (Apache 2.0), [Zig](https://ziglang.org) (MIT), [Capstone](https://www.capstone-engine.org) (BSD),
  [Unicorn](https://www.unicorn-engine.org) (GPL 2.0), [python-lz4](https://github.com/python-lz4/python-lz4) (BSD).

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
- The Fira Sans font, Medium and SemiBold (SIL OFL 1.1, `FiraSans-OFL.txt`).
