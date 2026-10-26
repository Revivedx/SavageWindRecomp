# Changelog

What changed in each Savage Wind release. The `test` branch holds what the next one will contain.

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
