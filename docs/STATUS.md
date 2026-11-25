# Status

[Back to the README](../README.md)

Estimates of where each part stands, grouped by area. "Tested in game" is how much of the game has been played through on Savage Wind so far.

The overall bar in the README is a weighted average. Each area has its share of the whole (the percentage in its heading). Each part has a weight inside its area: x1 an extra, x2 important, x3 essential. An area's value is the weighted mean of its parts, and the overall value the weighted mean of the areas. After changing a table, run `python tools/status_bars.py` to redraw the bars.

Testing glitches such as windbombs, Bullet Time Bounce (BTB), or inventory tricks? Use the [glitch compatibility tracker](glitch-compatibility.md) to record results by build and frame rate.

✅ done · 🔧 in progress · 🗺️ roadmap

### Core game (45%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| ✅ | **Recompiler** | ![100%](media/progress/100.svg) | x3 | The whole game (main, sdk and multimedia modules) recompiles to native code |
| 🔧 | **Horizon OS services** | ![95%](media/progress/95.svg) | x3 | Threads, files, controllers, applets and time: everything the game uses from start to end |
| 🔧 | **Maxwell shader translator** | ![90%](media/progress/90.svg) | x3 | Every shader seen so far translates to Vulkan |
| 🔧 | **Graphics (NVN on Vulkan)** | ![92%](media/progress/92.svg) | x3 | The whole game draws correctly, shadows included, on NVIDIA, AMD, Intel and Steam Deck. Alpha 6 fixed the wrong sky on AMD cards on Windows ([#359](https://github.com/Revivedx/SavageWindRecomp/issues/359)), transparent water ([#649](https://github.com/Revivedx/SavageWindRecomp/issues/649)) and the Gerudo Desert terrain spikes ([#855](https://github.com/Revivedx/SavageWindRecomp/issues/855)). Older GPUs still have open reports ([#589](https://github.com/Revivedx/SavageWindRecomp/issues/589)) |
| 🔧 | **Tested in game** | ![95%](media/progress/95.svg) | x3 | The whole game can be finished: the Shrine of Resurrection, the Great Plateau, the four Divine Beasts and the main story to Dark Beast Ganon, completed by players ([#335](https://github.com/Revivedx/SavageWindRecomp/issues/335), [#380](https://github.com/Revivedx/SavageWindRecomp/issues/380), [#419](https://github.com/Revivedx/SavageWindRecomp/issues/419), [#436](https://github.com/Revivedx/SavageWindRecomp/issues/436)), and a walk through every region on a 100% save file. As of Alpha 6 no known bug stops you from progressing. Some mechanics still break above 30 fps ([#425](https://github.com/Revivedx/SavageWindRecomp/issues/425)): lower the frame rate to 30 for that moment and [report what breaks](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose) |
| ✅ | **Sound and voices** | ![100%](media/progress/100.svg) | x2 | Mixed by the game SDK's own audio renderer, with room reverb |
| ✅ | **Game files setup** | ![100%](media/progress/100.svg) | x2 | Version check, copy, data image and code build, all from your own files |
| ✅ | **DLC** | ![100%](media/progress/100.svg) | x2 | *The Master Trials* and *The Champions' Ballad* from your own copy, set up with the game files or later ([The DLC](game-files.md#the-dlc)): Master Mode, the Trial of the Sword and the Travel Medallion work on a 100% save. The Champions' Ballad has been played through. |
| ✅ | **Game version 1.9.0** | ![100%](media/progress/100.svg) | x1 | The latest version and the supported one (its Thai text isn't offered yet). 1.8.2 is no longer supported: setup refuses it |
| ✅ | **Text and voice languages** | ![100%](media/progress/100.svg) | x1 | Any of the game's 15 text languages (16 with 1.9.0's Thai) with any of its 8 voice languages |
| ✅ | **Voices in another language: crash fix** | ![100%](media/progress/100.svg) | x1 | Fixed ([#2](https://github.com/Revivedx/SavageWindRecomp/issues/2)): the game now sets aside enough memory for voice banks bigger than its own, so any voice language works with any text language |

### Platforms and input (15%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| 🔧 | **Linux** | ![90%](media/progress/90.svg) | x3 | Builds and runs on x86-64 (from LAS Recomp); a steady 144 fps on an RTX 3070 Ti with a Ryzen 5 7600X, and no Linux-only issues so far |
| 🔧 | **Controllers** | ![95%](media/progress/95.svg) | x3 | Any controller SDL knows, by button position, confirmed by players. Every connected controller is open and the last one used is read, Steam Input pads are told apart, and camera speed and vibration strength are in Options > Controls ([#469](https://github.com/Revivedx/SavageWindRecomp/issues/469), [#540](https://github.com/Revivedx/SavageWindRecomp/issues/540), [#563](https://github.com/Revivedx/SavageWindRecomp/issues/563)). Next: camera speed set apart for each axis |
| ✅ | **Keyboard and mouse** | ![100%](media/progress/100.svg) | x3 | Every key and mouse button remappable, mouse camera |
| 🔧 | **PC handhelds** | ![85%](media/progress/85.svg) | x2 | Recognised by maker and model, with a PC Handheld preset, the screen's own size and 30, 40, 45 and 60 fps (up to the refresh rate on faster screens). Gyro through Handheld Companion's DSU server on the ROG Ally, Legion Go and others. Tested on the Steam Deck only |
| ✅ | **Motion controls (gyro)** | ![100%](media/progress/100.svg) | x2 | The controller's gyroscope and accelerometer (DualSense, DualShock 4, Switch controllers, Steam Deck, the controller behind Steam Input, DSU servers on Windows and Linux, a handheld's own sensors) for motion aiming and the tilt shrines; without one, the sticks, the keys or the mouse tilt them. Sensitivity, vertical sensitivity, inversion, Lean to turn and Motion source in Options > Controls. Reworked for Alpha 6: more controllers need testing ([#136](https://github.com/Revivedx/SavageWindRecomp/issues/136)) |
| 🔧 | **Button icons for your device** | ![90%](media/progress/90.svg) | x1 | The game's button prompts show your bound keys and mouse buttons, or Xbox / PlayStation / Nintendo / Steam Deck buttons, switching with the device you use ([#469](https://github.com/Revivedx/SavageWindRecomp/issues/469)) |
| ✅ | **Variable refresh rate (VRR)** | ![100%](media/progress/100.svg) | x1 | Video > VSync: On lets G-SYNC and FreeSync displays show every frame as soon as it's ready, a little under the refresh rate; Fast (the default), Adaptive and Off too. Tried on a VRR display |
| ✅ | **Window sizes and fullscreen** | ![100%](media/progress/100.svg) | x1 | The sizes your display can show; windowed, borderless or exclusive |
| ✅ | **Ultrawide** | ![100%](media/progress/100.svg) | x1 | Video > Fill wide screens (on by default): a window or screen wider than 16:9 (21:9, 32:9) shows more of the world to the sides instead of bars, and a taller one (16:10, 4:3) more above and below. The HUD stays 16:9 in the middle; the pause menu, the map, the loading screen and the start screen are shown 16:9 |

### Performance (15%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| ✅ | **Steady 60 fps in the open world** | ![100%](media/progress/100.svg) | x3 | Holds 60 fps in real play on the Great Plateau (RTX 3060, Ryzen 5 5600X); steady 120 fps (8 ms frames) is the next goal |
| ✅ | **Shader and pipeline cache** | ![100%](media/progress/100.svg) | x2 | Places you've already seen don't stutter again |
| ✅ | **Full shader precompilation** | ![100%](media/progress/100.svg) | x2 | Optional and on by default, at setup or later in Savage Wind Configuration: all of the game's ~14,000 shaders are translated and compiled ahead of time, and 11,615 pipelines from tours of every region with every graphics option are prepared before you play, so new places and effects don't stutter the first time. Problem reports carry your pipeline list, so the next versions prepare those too |
| ✅ | **Frame rate** | ![100%](media/progress/100.svg) | x2 | 60 fps by default, or 30, 72, 90, 120, 144, 165, 240, 360 no limit or your display's own rate, with the game at the same speed; menus, cutscenes and conversations too, and videos at their own speed |
| ✅ | **Graphics presets and Potato settings** | ![100%](media/progress/100.svg) | x2 | Video > Graphics preset: Potato, Original, High or Savage, plus an Advanced options page; draw distance and shadows below the console's for weaker PCs |
| ✅ | **Faster game code build** | ![100%](media/progress/100.svg) | x1 | The one-time setup takes about 15 minutes instead of about 40 (12 threads): the game code builds in about 6 minutes instead of 30 ([#43](https://github.com/Revivedx/SavageWindRecomp/pull/43), by [@adriadam10](https://github.com/adriadam10)) |
| ✅ | **Faster release build** | ![100%](media/progress/100.svg) | x1 | Profile-guided optimisation and ThinLTO for the runtime ([#801](https://github.com/Revivedx/SavageWindRecomp/pull/801), by Fayberr): +8-12% fps with FSR 3 on an RTX 3060 and a Ryzen 5 5600X |
| 🔧 | **GPU readings on AMD and Intel (Windows)** | ![90%](media/progress/90.svg) | x1 | The performance overlay's GPU use and VRAM on Windows: NVIDIA through its driver, AMD's load, clock, temperature and power through AMD's library ([#547](https://github.com/Revivedx/SavageWindRecomp/issues/547)), and any other GPU through the Windows counters (not tested on Intel by us) |
| 🔧 | **Built-in benchmark** | ![30%](media/progress/30.svg) | x1 | For developers: `tools/perf_routes.py` drives fixed routes that measure fps and frame times (PERF-ROUTES.md). Planned: the same from inside the game |

### Graphics options (12%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| ✅ | **Internal resolution and FSR 1** | ![100%](media/progress/100.svg) | x3 | Native resolution up to 4K, or AMD FSR 1 quality modes |
| 🔧 | **AMD FSR 3 + frame generation** | ![85%](media/progress/85.svg) | x3 | Video > Upscaler: FSR 3 and FSR 4 (AMD Radeon RX 9000) upscaling with a Native mode, their motion vectors on a thread of their own; shadows follow the upscaler. Video > Frame generation (FSR 3): a frame made between each two the game draws, about twice the frames on screen, with the HUD kept sharp, the horse and the Master Cycle Zero included. Works on Windows; frame generation isn't tried on Linux and the Steam Deck yet |
| ✅ | **Draw distance** | ![100%](media/progress/100.svg) | x2 | Video > Draw distance: Original (the game's own), Medium or High. Trees, rocks, buildings, enemies and villagers appear further out (1.5x at Medium, 1.6x at High, up to 2 km), buildings keep their full models further before their far versions take over, and distant landmarks such as Hyrule Castle and the terrain's distant trees stay drawn 1.5x as far. The grass reaches further too: the short grass 1.6x (Medium) or 2.5x (High), the tall grass about 1.15x or 2.45x. The terrain's own detail levels stay the game's ([#88](https://github.com/Revivedx/SavageWindRecomp/issues/88)) |
| 🔧 | **NVIDIA DLSS (RTX)** | ![90%](media/progress/90.svg) | x2 | Video > Upscaler: DLSS Super Resolution ([#602](https://github.com/Revivedx/SavageWindRecomp/pull/602), by Yiozolm) on NVIDIA RTX graphics cards, Windows: the CNN model on RTX 20 and 30, the transformer model on RTX 40 and later. Nothing of NVIDIA's is shipped or built in: Savage Wind Configuration downloads NVIDIA's DLSS library from NVIDIA's own repository. Not tried on RTX 40 and 50 by us |
| 🔧 | **Colour grading and HDR** | ![90%](media/progress/90.svg) | x2 | Options > Colour: presets, exposure, contrast, saturation, temperature, tint and LUTs (three CC0 ones included; your own .cube files and ReShade .png strips go in the `luts` folder), on the 3D image only: the HUD keeps its colours. HDR10 output on HDR displays: the game's tonemap keeps highlights above white, with a Vanilla or RenoDX PsychoV tone mapper (the BOTW mod's "RenoDRT" option), its exposure, highlights, shadows, contrast, saturation, highlight saturation, flare and gamma controls, peak/game/UI brightness, RenoDX's MacLeod-Boynton hue/purity transfer, SDR blend, vanilla saturation, bloom and gamut clamp; live HDR presets (Preset Off plus three saveable whole looks) and an Advanced fold in the overlay |
| 🔧 | **Effects on or off** | ![30%](media/progress/30.svg) | x2 | Video > Advanced options: bloom, depth of field and haze, the upscaler's Dynamic mode (dynamic resolution), and Video > Anti-aliasing turns the game's FXAA off. Planned for the rest, where BOTW has them: ambient occlusion, volumetric fog, grass (3 layers), character light, tone mapper, lens flare, cel shading, water, torch fire, screen-space reflections, global illumination and far lights |
| ✅ | **Anisotropic filtering and sharpening** | ![100%](media/progress/100.svg) | x1 | Ground and paths sharp at a distance (2x to 16x); AMD CAS sharpening of the 3D image, useful with FSR 1 |
| ✅ | **Higher-resolution shadows** | ![100%](media/progress/100.svg) | x1 | Sun shadows at the console's 896x896, or sharper at 2048, 4096 or 8192 (Video > Shadow resolution) |
| ✅ | **Field of view** | ![100%](media/progress/100.svg) | x1 | Video > Field of view: 70% to 150% of the game's own in play; cutscenes and conversations keep theirs |
| 🔧 | **Draw distance and detail** | ![80%](media/progress/80.svg) | x1 | Draw distance and shadows from Potato, below the console, to far beyond it, with presets and a render scale of 50% to 200%. Planned: a fixed level of detail (the most detailed models at any distance) |
| 🗺️ | **Graphics extras** | ![0%](media/progress/0.svg) | x1 | Later: ambient occlusion, volumetric fog and depth of field |

### Player features (13%)

| | Part | Progress | Weight | What it means |
|---|---|---|---|---|
| 🔧 | **Mods** | ![20%](media/progress/20.svg) | x3 | Language packs: `lang/<language>.txt` in the data folder is read over the built-in translations ([#557](https://github.com/Revivedx/SavageWindRecomp/issues/557)). Planned: a loader for other mods |
| ✅ | **Settings in the game's Options** | ![100%](media/progress/100.svg) | x2 | Video, audio, controls and language pages drawn by the game itself |
| ✅ | **Save profiles and backups** | ![100%](media/progress/100.svg) | x2 | Settings > Saves: complete sets of saves to switch between ([#521](https://github.com/Revivedx/SavageWindRecomp/issues/521)), a backup at each start with the last 5 kept ([#537](https://github.com/Revivedx/SavageWindRecomp/issues/537)), and export to Wii U / Cemu ([#549](https://github.com/Revivedx/SavageWindRecomp/issues/549)) |
| 🔧 | **Accessibility and assists** | ![70%](media/progress/70.svg) | x2 | Options > Accessibility: colour vision filters (Protanopia, Deuteranopia, Tritanopia), God mode, One-hit kill, damage dealt and taken, weapon wear levels, infinite arrows and stamina ([#374](https://github.com/Revivedx/SavageWindRecomp/issues/374), [#534](https://github.com/Revivedx/SavageWindRecomp/issues/534)); dialogue text speed, menu cursor speed and HUD Off in Options. All of it explained in [Cheats and assists](cheats-and-assists.md). Planned: text size and toggles in place of held buttons; enemy and Link health on screen; no fall damage, health that refills over time, and speed multipliers |
| ✅ | **Direct start + Configuration** | ![100%](media/progress/100.svg) | x1 | savagewind.exe goes straight into the game; Savage Wind Configuration has the rest |
| ✅ | **Menus in 13 languages** | ![100%](media/progress/100.svg) | x1 | Savage Wind's own screens follow the text language. Hebrew, right to left, which the game has no text language for: `SAVAGEWIND_MENU_LANG=he` |
| ✅ | **Cheats** | ![100%](media/progress/100.svg) | x1 | Overlay (F1) > Cheats and the console: items by category (critters and fairies included), infinite rupees or an exact count, and weather ([#423](https://github.com/Revivedx/SavageWindRecomp/issues/423), [#535](https://github.com/Revivedx/SavageWindRecomp/issues/535), [#545](https://github.com/Revivedx/SavageWindRecomp/issues/545)); a [guide](cheats-and-assists.md) |
| ✅ | **Free camera and photo mode** | ![100%](media/progress/100.svg) | x1 | Home or both stick clicks: a free camera with the HUD hidden. F8 filters, [ ] the hour of the day, F7 a picture, and keyframe camera paths for videos (`K` add keyframe, `Backspace` remove, `C` clear, `P` play, `O` loop, `-/=` speed; `Y`/`X`/`B` on controller) ([#318](https://github.com/Revivedx/SavageWindRecomp/issues/318), [#554](https://github.com/Revivedx/SavageWindRecomp/issues/554)) |
| 🔧 | **amiibo rewards without figures** | ![50%](media/progress/50.svg) | x1 | Overlay (F1) > amiibo: a virtual reader for your own figure dumps, nothing of a figure or a key is included ([#422](https://github.com/Revivedx/SavageWindRecomp/issues/422)). Planned: the rewards without any figure |
| 🔧 | **Teleport with bookmarks** | ![40%](media/progress/40.svg) | x1 | The console (developer mode) teleports to any shrine, tower, stable, village, landmark or Korok by number or name ([Teleport places](teleport-places.md)), and `pos` copies a line to come back to. Planned: bookmarks in the overlay |
| 🗺️ | **Achievements** | ![0%](media/progress/0.svg) | x1 | Planned: achievements from your save's progress, from start to finish ([#236](https://github.com/Revivedx/SavageWindRecomp/pull/236)) |
