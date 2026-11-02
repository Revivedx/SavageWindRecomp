# Changelog

What changed in each Savage Wind release. The `test` branch holds what the next one will contain.

## Alpha 4.1 (0.4.1-alpha.4) - 2026-11-01

**A quick fix update for Alpha 4.** Extract it over your Alpha 4 folder (Windows) or use the new AppImage (Linux): your
game files, the game code built at setup, saves and settings stay, with no new setup.

- **AMD graphics on Windows:** no more "GPU page fault" and device lost at boot or in the first cutscene on AMD's
  26.10 drivers (RX 9070 XT, RX 9060 XT, RX 7000 cards). AMD's shader compiler misread the game's memory past 2 GB in
  one kind of shader; Savage Wind now hands it the memory in two parts, each under 2 GB. Everything stays on, the GPU
  overlap included, so `SAVAGEWIND_GPU_OVERLAP=0` is no longer needed ([#331](https://github.com/Revivedx/SavageWindRecomp/issues/331), [#334](https://github.com/Revivedx/SavageWindRecomp/issues/334),
  [#340](https://github.com/Revivedx/SavageWindRecomp/issues/340), [#339](https://github.com/Revivedx/SavageWindRecomp/issues/339); by conongnang). AMD players rebuild their shader cache once.
- **Crash with the mouse in menus:** opening the inventory, switching Adventure Log tabs or changing weapons with the
  mouse could crash with `c0000005 accessing 0xfff00000001` in the graphics driver. The pointer's highlight box was
  drawn while the frame was still being recorded on another thread ([#333](https://github.com/Revivedx/SavageWindRecomp/issues/333), [#350](https://github.com/Revivedx/SavageWindRecomp/issues/350)).
- **Controller on the map:** the controller takes map navigation back from the mouse as soon as you use it, and
  opening the map with the controller no longer starts it under a still mouse ([#336](https://github.com/Revivedx/SavageWindRecomp/pull/336); by YmirxUwu).
- **Quick weapon selector:** with the mouse, the selector now uses only the wheel, so moving or clicking the mouse no
  longer picks a different item ([#344](https://github.com/Revivedx/SavageWindRecomp/pull/344); by YmirxUwu).
- **Game Over screen:** the mouse now works on Continue and Quit, and on Quit's confirmation
  ([#347](https://github.com/Revivedx/SavageWindRecomp/pull/347); by YmirxUwu).

## Alpha 4 (0.4.0-alpha.4) - 2026-10-31

**The biggest update so far.** Alpha 4 adds the two DLC, runs much faster on discrete GPUs, brings a draw distance
setting, colour grading, HDR10 and AMD FSR 3 frame generation, imports Wii U / Cemu saves, and fixes most of what
players reported on Alpha 3, including the memory that grew over long sessions.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of the game, extracted from **your own**
> Nintendo Switch. Savage Wind provides no game files, keys, firmware, dumping or decryption tools, or instructions to
> obtain any of them. The game code is recompiled on your machine, from your files, for your own use only: don't
> share it. This is a fan project, not affiliated with or endorsed by Nintendo.

### Highlights

- **Downloadable content:** The Master Trials and The Champions' Ballad, from your own DLC files. Both are optional.
- **Faster on discrete GPUs:** the game's geometry is kept in video memory instead of being read over PCIe on every
  draw. On an RTX 3060 at 1440p, 72 to 105 fps standing and 70 to 108 fps turning; at 1080p, 81 to 124 fps. 4-6 GB
  cards get it too, in chunks (about +40-50 % on a simulated 4 GB card).
- **CPU and GPU work in parallel**, the driver calls run on a thread of their own, and the game copies about 13 times
  less constant data to the GPU every frame.
- **Draw distance** (Video > Draw distance: Original, Medium, High): trees, rocks, buildings, enemies and grass appear
  further out, and the distant trees fade in as smoothly as at Original.
- **Colour grading** (Options > Colour): presets, exposure, contrast, saturation, temperature and LUTs, with Savage
  Wind's own look offered at setup.
- **HDR10 output**, **AMD FSR 3 frame generation** (Windows) and **TAA**.
- **Import Wii U / Cemu saves** from the new Settings > Saves tab. It backs up your current saves first and asks
  before it replaces them.

### Added

- **Shrine tilt puzzles without a gyro** ([#235](https://github.com/Revivedx/SavageWindRecomp/issues/235), [#272](https://github.com/Revivedx/SavageWindRecomp/issues/272)): while you control a tilt apparatus, tilt it with I/K, J/L and
  U/O (P recentres), drag it with the mouse, or use both sticks on a pad without motion sensors (R3/L3 recentre).
  Switches in Options > Controls; the keys can be rebound.
- **In-game console** ([#284](https://github.com/Revivedx/SavageWindRecomp/issues/284), [#285](https://github.com/Revivedx/SavageWindRecomp/issues/285)): F12 or the key left of 1. `report` and `pos` copy your version, hardware,
  settings and exact place to the clipboard, ready to paste into an issue. Developer commands (teleport, time, input,
  measurements) need `SAVAGEWIND_DEV=1` or a local control socket ([#286](https://github.com/Revivedx/SavageWindRecomp/issues/286)); see `docs/CONTROL.md`.
- **Steam Deck gyro** in the Linux AppImage through SteamDeckGyroDSU, for bow aiming and motion puzzles ([#281](https://github.com/Revivedx/SavageWindRecomp/issues/281)).
- Video > Monitor and Video > Fullscreen mode (borderless or exclusive).
- Window size dropdown, 16:10 fill, and smaller window sizes.
- **Mouse pointer in the menus** ([#234](https://github.com/Revivedx/SavageWindRecomp/issues/234)): the game's own cursor follows the mouse in the pause menu, inventory, map
  and Options. Click to confirm, right-click to go back, drag to turn Link, edge-scroll the map, and click shop
  counts, dialog choices and the Q / E tabs. Options > Controls has a switch for it.
- Native mouse camera, with 1:1 mouse look instead of stick emulation.
- Keys 1-4 for the quick menus. Unbound arrow keys and Enter work in the title, pause and map menus.
- The performance overlay shows the active upscaler and Link's coordinates, and works on any GPU.
- `savagewind.log` starts with the Savage Wind version and source, names each cutscene as it starts, and notes where
  Link is every 30 seconds while he moves, so a log attached to an issue says where the bug happened.
- The game data is read from the romfs folder: no second 15 GB copy at setup.

### Fixed

- **Memory over long sessions ([#237](https://github.com/Revivedx/SavageWindRecomp/issues/237)):** memory the game frees goes back to the system, and finished game threads,
  their timers and closed files are released. Before, every death and area reload left memory and handles behind, and
  long sessions could run out.
- **Crashes:**
  - GTX 10-series (Pascal) at startup ([#36](https://github.com/Revivedx/SavageWindRecomp/issues/36));
  - Intel graphics on a render pass with nothing to draw into ([#132](https://github.com/Revivedx/SavageWindRecomp/issues/132), [#302](https://github.com/Revivedx/SavageWindRecomp/issues/302));
  - the sampler limit ([#239](https://github.com/Revivedx/SavageWindRecomp/issues/239));
  - the Steam Deck on Continue;
  - Adreno on startup ([#207](https://github.com/Revivedx/SavageWindRecomp/issues/207)).
- **Freezes:**
  - the King Rhoam cutscene in exclusive fullscreen ([#224](https://github.com/Revivedx/SavageWindRecomp/issues/224));
  - Master Kohga's entrance at high frame rates ([#196](https://github.com/Revivedx/SavageWindRecomp/issues/196));
  - the start screen's Settings and Game Files ([#276](https://github.com/Revivedx/SavageWindRecomp/issues/276)).
- **AMD and Intel on Windows** are back on the full Vulkan setup ([#85](https://github.com/Revivedx/SavageWindRecomp/issues/85)); RADV fixes on Linux ([#90](https://github.com/Revivedx/SavageWindRecomp/issues/90), [#228](https://github.com/Revivedx/SavageWindRecomp/issues/228)).
- **Rendering:**
  - FOV shadows ([#148](https://github.com/Revivedx/SavageWindRecomp/issues/148), [#162](https://github.com/Revivedx/SavageWindRecomp/issues/162), [#168](https://github.com/Revivedx/SavageWindRecomp/issues/168), [#203](https://github.com/Revivedx/SavageWindRecomp/issues/203), [#264](https://github.com/Revivedx/SavageWindRecomp/issues/264));
  - beach sprites ([#147](https://github.com/Revivedx/SavageWindRecomp/issues/147));
  - NPC speech bubbles ([#84](https://github.com/Revivedx/SavageWindRecomp/issues/84));
  - particle halos and leaf glow ([#129](https://github.com/Revivedx/SavageWindRecomp/issues/129), [#219](https://github.com/Revivedx/SavageWindRecomp/issues/219));
  - frame generation flicker;
  - the black flicker when selecting weapons ([#173](https://github.com/Revivedx/SavageWindRecomp/issues/173));
  - purple specks on the Steam Deck;
  - compressed render targets on integrated GPUs.
- **Wide screens:**
  - the pause menu stays centred at 16:9 ([#238](https://github.com/Revivedx/SavageWindRecomp/issues/238));
  - the world map's picture isn't stretched ([#282](https://github.com/Revivedx/SavageWindRecomp/issues/282), [#253](https://github.com/Revivedx/SavageWindRecomp/issues/253));
  - loading screens keep 16:9 ([#98](https://github.com/Revivedx/SavageWindRecomp/issues/98)).
- **Gameplay:**
  - arrows fly the same at any frame rate ([#144](https://github.com/Revivedx/SavageWindRecomp/issues/144));
  - cucco jumps above 60 fps ([#161](https://github.com/Revivedx/SavageWindRecomp/issues/161));
  - gyro aiming past 90° ([#164](https://github.com/Revivedx/SavageWindRecomp/issues/164)) and motion aiming ([#220](https://github.com/Revivedx/SavageWindRecomp/issues/220));
  - horse names at stables ([#271](https://github.com/Revivedx/SavageWindRecomp/issues/271));
  - Japanese voices with other subtitle languages ([#169](https://github.com/Revivedx/SavageWindRecomp/issues/169));
  - battle music ([#145](https://github.com/Revivedx/SavageWindRecomp/issues/145));
  - the DLC-merged RomFS ([#188](https://github.com/Revivedx/SavageWindRecomp/issues/188)).
- **Controllers** no longer act in the game while its window is in the background ([#301](https://github.com/Revivedx/SavageWindRecomp/issues/301)).
- **Capture software:** OBS's Game Capture no longer shows black or broken frames ([#293](https://github.com/Revivedx/SavageWindRecomp/issues/293)): frames are presented on the
  game's own queue (the same or 1-3 fps more on an RTX 3060).
- **Moving platforms** at high frame rates: Vah Medoh's floor, lifts and gates no longer jolt ([#209](https://github.com/Revivedx/SavageWindRecomp/issues/209)), and shield-surf
  jumps keep their boost at 120 fps ([#305](https://github.com/Revivedx/SavageWindRecomp/issues/305)).
- **HUD text** no longer jumps for a frame above 60 fps ([#263](https://github.com/Revivedx/SavageWindRecomp/issues/263)).
- **Linux:** the overlay's GPU numbers come from the GPU that renders, on PCs with two ([#315](https://github.com/Revivedx/SavageWindRecomp/issues/315)); RADV on GCN chips
  (Vega, Renoir, Polaris) copies render targets instead of feedback loops it drew wrong ([#90](https://github.com/Revivedx/SavageWindRecomp/issues/90), likely [#291](https://github.com/Revivedx/SavageWindRecomp/issues/291)).
- **Setup:**
  - a failed game code build says why ([#113](https://github.com/Revivedx/SavageWindRecomp/issues/113), [#123](https://github.com/Revivedx/SavageWindRecomp/issues/123));
  - mod modules are skipped;
  - the Control Flow Guard crash of the code generator on recent Windows 11: it now starts with CFG off for that
    process only ([#113](https://github.com/Revivedx/SavageWindRecomp/issues/113), [#266](https://github.com/Revivedx/SavageWindRecomp/issues/266));
  - game files without their cutscene videos (the Movie folder) are reported incomplete instead of crashing at the
    first cutscene ([#292](https://github.com/Revivedx/SavageWindRecomp/issues/292));
  - "out of GPU memory" explains itself ([#117](https://github.com/Revivedx/SavageWindRecomp/issues/117)).

### Known

- **Draw distance:** bushes can still pop in at High. Actors stop at 1.15x (Medium) and 1.3x (High) because the
  game leaves some out past that in dense places.
- **AMD's legacy Windows driver** (RX 400/500, Vega) still copies render targets instead of using feedback loops, so
  it's slower ([#261](https://github.com/Revivedx/SavageWindRecomp/issues/261)).
- **Memory:** some small texture objects are still never freed, about 1-2 MB per reload.
- **FSR 3 upscaling** stays hidden: on today's code it costs more than it saves.
- **Stutter the first time a place is seen:** the setup precompiles the shaders, but each new combination of shaders
  and drawing state is put together the first time the game draws it. They're saved and load at the next start, so
  it fades as you play.
- **Linux:** the geometry copy in video memory is Windows-only for now.

### Updating

Replace the files of your Alpha 3 folder with the zip's (Windows), or use the new AppImage (Linux). Your game files,
saves and settings stay. The start screen asks you to build the game code again (Set up game files) once, and the
shader cache is rebuilt as you play.

Thanks to everyone who sent pull requests, reports and logs: Adrià Franch, Benjamin Todd, Boyce Yi, Cainã Costa,
conongnang, Demetter, Fayber, fgabe, Igorms27, Marllon Menezes, Matthew Doe, Moroni Granja, Phosquitos, Preetish
Choudhary, sabeliam, Tchaika1, Tempus3214, wuruoye, Wuawydu, xiaoxue508, YmirxUwu, and more.

## Alpha 3 hotfix (0.3.1-alpha.3, Windows) - 2026-10-25

**For AMD and Intel graphics on Windows.** Since Alpha 3, AMD's and Intel's Windows drivers showed only a black window,
the start screen included, where Alpha 2 drew ([#85](https://github.com/Revivedx/SavageWindRecomp/issues/85)). On
those drivers Savage Wind goes back to Alpha 2's Vulkan setup: three newer features Alpha 3 turned on there
(maintenance5, graphics pipeline libraries and attachment feedback loops) stay off.

- **Nothing else changes.** NVIDIA on Windows and everything on Linux run exactly as in 0.3.0; Linux keeps the 0.3.0
  AppImages.
- **Updating:** replace the files of your 0.3.0 folder with the zip's. Your game files, the game code built at setup,
  saves and settings stay: no new setup.
- **Help us find the cause:** we have no Windows PC with AMD graphics. Each of the three features can be turned back on
  for a test (`set SAVAGEWIND_VK_M5=1`, `set SAVAGEWIND_VK_GPL=1` or `set SAVAGEWIND_VK_FEEDBACK_LOOP=1` in a command
  prompt, then start `savagewind.exe` from it). Tell us in #85 which one brings the black window back, with the
  `savagewind.log`; the other two make the game faster and can then come back on for your GPU.

## Alpha 3 (0.3.0-alpha.3) - 2026-10-24

**The first release for Linux, with a highly experimental build for ARM.** Alpha 3 supports game version 1.9.0. It
adds ultrawide, a field of view slider, motion controls and shader precompilation, builds the game code about five
times faster, and fixes the bugs players reported on Alpha 2.

> [!IMPORTANT]
> **No game files are included.** You need your own legally obtained copy of the game, extracted from **your own**
> Nintendo Switch. Savage Wind provides no game files, keys, firmware, dumping or decryption tools, or instructions to
> obtain any of them, and it never decrypts anything. The game code is recompiled on your machine, from your files, for
> your own use only: don't share it. This is a fan project, not affiliated with or endorsed by Nintendo.

### Downloads

| Build | For | Status |
|---|---|---|
| Windows (zip) | Windows 10 or later, x86-64 | Supported |
| Linux x86-64 (AppImage) | glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, SteamOS...) | Supported; tested on desktop Linux and the Steam Deck |
| Linux aarch64 (AppImage) | ARM64 Linux with a Vulkan 1.3 driver (Asahi Linux, Snapdragon laptops, Raspberry Pi 5 and other boards) | **Highly experimental** |

> [!WARNING]
> **The aarch64 AppImage is highly experimental.** It is cross-compiled on x86-64 and has never been run on real ARM
> hardware, only its start screen under emulation. It may not set up, start or run at all, and Android isn't
> supported. If you try it, please report what happened, including the details listed in
> [docs/linux.md](docs/linux.md) ("Testing on ARM").

### Game version

- **1.9.0 is the supported version**, the latest one. 1.8.2 still sets up and runs, but a bug is only fixed if it
  also happens on 1.9.0.
- Thai is offered as a text language with 1.9.0.
- The setup checks that the RomFS matches the ExeFS's version.

### New

- **Ultrawide** (#65): with Video > Fill wide screens (on by default), a window or screen wider than 16:9 (21:9,
  32:9) shows more of the world to the sides instead of black bars.
  - The HUD stays 16:9 in the middle. Fades, the loading screen and the menus' backgrounds fill the window.
  - The pause menu, the map and the start screen are shown 16:9, as they are laid out for it.
  - Narrower shapes (16:10, 4:3) keep the 16:9 picture with bars.
- **Field of view**: Video > Field of view, 70% to 150% of the game's own, in play only. Cutscenes and conversations
  keep their framing.
- **Motion controls**: the game's motion aiming and tilt puzzles from the controller's gyroscope (DualSense,
  DualShock 4, Switch controllers, Steam Deck), or from the mouse while holding T. Sensitivity and inversion are in
  Options > Controls. The gyroscope's drift at rest is learnt and removed.
- **Shader precompilation**: the setup can translate and compile all of the game's ~14,000 shaders ahead of time, so
  new places and effects don't stutter. It uses at most about 4 GB of memory while it runs (#56).
- **Button icons for your device**: the game's button prompts, including the HUD's action prompt, gliding and the
  d-pad, show your bound keys and mouse buttons, or Xbox, PlayStation or Nintendo buttons. They switch with the device
  you use.
- **Stick dead zone** for each stick (Options > Controls, 10% by default). It fixes the map's and menus' sticks and
  stick drift (#27).
- **Video options**:
  - VSync (On, Fast, Adaptive, Off) for G-SYNC and FreeSync displays;
  - anisotropic filtering;
  - AMD CAS sharpening;
  - shadow resolution up to 8192.
- **Window size**: your display's own size is listed first, marked "native", and the list wraps around at both ends
  (#59).
- **CPUs without AVX2 / FMA** (#9, #63): on such a CPU the setup builds the game code in a compatible mode, which runs
  slower. The minimum is now a CPU with SSE4.2 (2008 or newer).

### Faster

- **Game code build in about 6 minutes instead of 30** on a 12-thread CPU (#43).
- **Shaders translate and compile about 3x faster** (#38, #48, #49).
- **Steadier frames**: the open world holds 60 fps. Command lists and presents run on a thread of their own.
- **Menus, cutscenes and conversations run at the chosen frame rate**, as with Cemu's FPS++. The menus' cursor and
  animations keep their speed at any frame rate.
- **Pre-rendered movies** play at their own speed while the game keeps its frame rate.

### Fixed

- Trees no longer explode or show a cut ring (#16, #44).
- The pre-rendered cutscenes (the towers' and others) play, with the right picture, instead of crashing (#6, #62).
- Blocky glow on the teleport streaks, glowing weapons and Sheikah lights.
- A crash with the horse whistle (#22).
- Voices in another language than the text (#2).
- The second save's black thumbnail, the black pause and weapon-wheel backgrounds, the flat image after Continue, and
  the sun's missing glare.
- Simplified and Traditional Chinese showed Japanese (#69).
- Command record memory grew with every load (#74, #76).
- The setup now finds the game files in folders with accents or other scripts, and in RomFS folders with other names
  (#66).
- A lost GPU now says what it means and what to try, instead of "sr failed: -4" (#68).
- Denormals with flush-to-zero set were kept instead of flushed (#34).
- `savagewind.ini` edited with Notepad (CRLF line ends) or with trailing spaces is read correctly.
- The Downloadable Content entry is gone from the title menu until DLC is supported (#19).

### Linux and the Steam Deck

- Savage Wind builds on Linux with the SDKs distributions ship: VMA bundled, SDL3 3.2 and older Vulkan headers (#10).
- On AMD's RADV driver, a GPU hang is fixed, feedback loops work, and the game's memory is shared with the GPU as a
  dma-buf.
- On the Steam Deck the Great Plateau went from 15 to a steady 30 fps.

### Known issues

- **Played so far:** the Shrine of Resurrection, the Great Plateau, and a walk through every region on a 100% save
  file. The main quests and the Divine Beasts aren't tested yet, so expect bugs there.
- **Ultrawide is new**: BOTW has many screens and effects we haven't seen at 21:9 or 32:9 yet. If something is cut
  off, misplaced or stretched, please open an issue with a screenshot.
- At high frame rates some cutscenes and conversations end a little sooner than on the console, because a few of
  their waits count frames.
- The faint Y / A / B letters around the gliding prompt don't follow the device yet (Alpha 4).
- GTX 1050 laptops with an Intel GPU next to it (Optimus) can crash at startup; we're still investigating (#36).
  Lower-end GPUs such as the MX130 are too slow for now.
- Magenta patches on some AMD GPUs on Windows aren't confirmed fixed (#26, #59).
- The compatible mode for CPUs without AVX2 hasn't been tested on such a CPU yet.
- DLC isn't supported.

### Coming from Alpha 2

Extract Alpha 3 to a new folder and set the game files up again with your 1.9.0 files: the game code has to be built
again. To keep your progress, copy your `save` folder over.

### Thanks

Thanks to everyone who reported bugs, and to the contributors whose work is in this release:

- **@fgabe**: menus and cutscenes at any frame rate, pre-rendered movies, the stick dead zone, the gyroscope's bias,
  the blocky glow fix and Linux testing.
- **@adriadam10**: the faster game code build, the faster shader translation and the precompilation's memory budget
  (#43, #38, #48, #49, #56).
- **@Igorms27**: game code for CPUs without AVX2 / FMA (#63).
- **@xiaoxue508**: the Chinese language codes and the command record memory investigation (#69, #76).
- **@moronigranja**: building with the SDKs Linux distributions ship (#10).

## Alpha 2 (0.2.0-alpha.2) - 2026-10-21

- The open world: the Great Plateau with its terrain, water, blue sky, shadows and HUD.
- 60 fps by default, or 30, 90, 120 or no limit, with the game at the same speed.
- Native resolution up to 4K or AMD FSR 1; window sizes your display supports, windowed, borderless or exclusive
  fullscreen.
- Settings inside the game's own Options screen; Quit asks with the game's own dialog.
- `savagewind.exe` starts the game once it's set up; Savage Wind Configuration has the setup and the settings.
- A shader and pipeline cache, so places you've already seen don't stutter again.

## Alpha 1 (0.1.0-alpha.1) - 2026-10-17

- The title screen and the opening in the Shrine of Resurrection, lit, with sound, voices and saves, at 30 fps.
- Text and voices in different languages.
- Menus in the game's 11 languages.
- Keyboard and mouse with remappable keys and a mouse camera, and any controller SDL knows.
- An overlay menu and a performance overlay (F10).
