# FAQ

[Back to the README](../README.md)

This page goes from the practical to the personal: first how to get the game running and what is known to be broken,
then how the recompilation works, and last why the project is the way it is.

> [!IMPORTANT]
> **Issues about something covered here are closed right away**, with a comment saying why. If you tried what this page
> says and the problem is still there on the latest release, say so and attach `savagewind.log`.

**[Getting it running](#getting-it-running)** ·
**[Crashes](#crashes)** ·
**[Graphics](#graphics)** ·
**[Performance](#performance)** ·
**[Controls](#controls)** ·
**[Known issues](#known-issues)** ·
**[Reporting a problem](#reporting-a-problem)** ·
**[Other settings](#other-settings)** ·
**[How do I get the game files?](#how-do-i-get-the-game-files)** ·
**[The recompilation](#the-recompilation)** ·
**[Platforms and requests](#platforms-and-requests)** ·
**[Philosophy](#philosophy)** ·
**[Who is behind this?](#who-is-behind-this)**

---

## Getting it running

### Which version of the game do I need?

**1.9.0**, and only 1.9.0. Support for 1.8.2 ended with Savage Wind 0.5.1: if your copy is older, update it on your own
console and extract it again. [Why only one version?](#why-only-one-version-of-the-game)

### What do the game files need to look like?

Folders named `exefs/` and `romfs/`, already extracted and decrypted. Savage Wind doesn't decrypt anything and doesn't
accept a cartridge image or an installation package. The exact layout, and how to add the DLC, are in
[Game files](game-files.md).

### Setup says "No game files found in that folder"

Check that the folder you chose directly contains `exefs/` and `romfs/`, and not one more folder that holds them.

If you are reusing an install, extract the new Savage Wind in the same place and accept without choosing a folder: it
reads the `extracted` folder that is already there. If that fails, use a separate folder for the new Savage Wind and
copy `extracted` into it ([#590](https://github.com/Revivedx/SavageWindRecomp/issues/590)).

### "The game code has no address for opt_two_redraw", or setup says the version is wrong

Your game files are not version 1.9.0. Update your copy on your console and extract it again. We can't help you get a
newer copy, and asking for one in an issue gets the comment removed.

### Setup says "It has N files; with the update applied it has 41062"

Your extracted RomFS is incomplete: a complete 1.9.0 RomFS, with the update applied over the base game, has 41,062
files. Setup refuses fewer, because the game crashes as soon as it reads a missing file. Extract your copy again. How you
extract it is up to you, and we can't help with that part ([why](#how-do-i-get-the-game-files)).

### The loading screen never ends after boarding a Divine Beast

Your DLC is merged into `romfs`. Set the game up again with the DLC in its own folders, as
[Game files](game-files.md#the-dlc) shows: with it merged, the loading screen after boarding a Divine Beast never ends.
Keeping the DLC separate fixes it, and your saves stay.

### Setup stops at 5%, 10% or "The game code didn't build"

That step runs a compiler over your files, so it needs free space and a working toolchain. Check that:

- you have about 20 GB free on that drive;
- Savage Wind is in a folder you can write to, not Program Files or a synced folder such as OneDrive;
- your antivirus didn't quarantine anything from the Savage Wind folder.

If it still fails, [open an issue](#reporting-a-problem) with `savagewind.log`
([#618](https://github.com/Revivedx/SavageWindRecomp/issues/618),
[#669](https://github.com/Revivedx/SavageWindRecomp/issues/669)).

### How long does setup take?

About 15 minutes on a 12-thread CPU, and longer on fewer threads: a Steam Deck needs around 15 for the game code
alone. It only happens once, and again only if an update changes the game code.

### Do I have to extract the game again after updating Savage Wind?

No. The files you set up stay in the Savage Wind folder. An update can ask you to rebuild the game code from them, but
it doesn't need your original copy again.

### How do I update?

From 0.5.1 on, Savage Wind updates itself in one click from Savage Wind Configuration, on Windows and in the Linux
AppImages. If you are on 0.5.0 or older, install the latest version by hand once, by extracting the zip over the old folder
or by using the new AppImage. Your game files, saves and settings stay.

The first start of a new version empties the shader caches and precompiles them again, and it rebuilds the game code
only if that version needs it.

### Why is the first time I play slow or stuttery?

Shaders are built for your graphics card the first time they are needed. The precompilation step covers most of them,
and the rest appear as short stutters that go away once they are cached. On NVIDIA, a small driver cache can throw this
work away between sessions: in NVIDIA Control Panel > Manage 3D settings, raise **Shader Cache Size**
([#468](https://github.com/Revivedx/SavageWindRecomp/issues/468)).

### How much disk space and memory does it need?

About 20 GB of disk and 16 GB of RAM.

### Where are my saves? Can I bring saves from the Wii U or Cemu?

See [Saves](saves.md). Wii U and Cemu saves can be imported from **Savage Wind Configuration > Settings > Saves**.

### Can I keep the game files on another drive?

Yes: [Moving the data folder](data-folder.md).

---

## Crashes

### "Savage Wind stopped because of an error"

Attach the log and your zipped `save` folder as described in [Reporting a problem](#reporting-a-problem). The numbers in
the message are useful to us, even when they mean nothing to you.

Before reporting, check that you are on the latest release and on game version 1.9.0. Many old crashes are already
fixed.

### Crashes with `exception c0000005` on an Intel GPU or a GPU that shares system memory

Close the game, open `savagewind.ini` (next to `savagewind.exe`) and set:

```
sparse_heap = off
```

Then play again. If it still crashes, post the `savagewind.log` of that run on your issue
([#619](https://github.com/Revivedx/SavageWindRecomp/issues/619),
[#596](https://github.com/Revivedx/SavageWindRecomp/issues/596)).

### "Vulkan device lost" or "the GPU stopped responding"

Update your graphics driver first. Then lower the resolution and the shadow quality: running out of video memory is one
of the known causes on cards with little of it. If it keeps happening, report it with your log.

### It crashes while saving

Another program may be holding the save files, usually OneDrive or an antivirus. Keep the Savage Wind folder out of
synced folders and add it to your antivirus exceptions.

### It crashes after playing for hours

Long sessions are still being worked on, and memory use can grow as you play
([#640](https://github.com/Revivedx/SavageWindRecomp/issues/640)). Save often during long sessions, and tell us how
long you played and attach the log.

### A cutscene froze or broke

Set the frame rate to 30 fps for that scene, then raise it afterwards. Then tell us which scene it was on
[#425](https://github.com/Revivedx/SavageWindRecomp/issues/425).

### The game seems to keep running after I close it

Check your task manager for `savagewind.exe` and end it if it's there. If it comes back or won't end, report it.

---

## Graphics

### The window stays black on Windows with an AMD card

From 0.5.1 on, the first start of a new version tries to clear AMD's shader cache once. If it still happens, reset it by
hand:
in AMD Software: Adrenalin Edition, go to **Gaming > Graphics > Advanced** and click **Reset Shader Cache**, then start
Savage Wind again. AMD's driver can keep a damaged cache from an older version. Renaming the Savage Wind folder also
gives the driver a fresh cache.

### The sky is black, dark or wrong on my AMD card

Still being looked at. It happens on AMD's Windows driver with RX 6000 cards and older (Vega included), not on newer
cards or on Linux, and it can change from one start to the next. The 0.5.2 change for it did not fix it. Please follow
the steps on [#359](https://github.com/Revivedx/SavageWindRecomp/issues/359) and add your card, driver, screenshots and
logs there.

### The water is white, milky or transparent

Milky water after a loading screen was fixed for Alpha 5. Transparent water is still open
([#649](https://github.com/Revivedx/SavageWindRecomp/issues/649)). If you see either on the latest version, add your
GPU, driver and position.

### Where are FSR 3 and FSR 4 upscaling?

They are held back. They work, but the motion vectors they need cost about 20% of the frame rate on the render thread
(130 fps native, 104 with FSR 3, on an RTX 3060 at 1440p). This needs a redesign of the motion vectors, planned for
Alpha 6. FSR 1 upscaling is available, and FSR 3 frame generation too.

### Does frame generation work everywhere?

It works on Windows. Linux and the Steam Deck aren't tried yet. It adds a cost of its own, and on some systems it is
slower instead of faster ([#635](https://github.com/Revivedx/SavageWindRecomp/issues/635)). Turn it off if that happens.
Don't combine it with the NVIDIA app's or your driver's own frame generation.

### Why does TAA cost so much?

It uses the same motion vectors as FSR 3, so it has the same cost, with the same redesign planned
([#654](https://github.com/Revivedx/SavageWindRecomp/issues/654)).

### Which GPU does it use? Can I choose?

See [Choosing the GPU](#choosing-the-gpu).

---

## Performance

### My PC is weak. Where do I report how it runs?

On the pinned issue: **[#589, performance or graphical issues on integrated or older GPUs](https://github.com/Revivedx/SavageWindRecomp/issues/589)**.
Please don't open a new issue for it. Attach:

- your `savagewind.log`, from a run where you got to the world;
- your CPU, GPU and amount of RAM;
- your settings and window size;
- where you were in the game.

In the game, press F12 (or the key below Esc, left of 1), type `report`, and paste what it copies. This helps a lot.

Reports like these decide what gets optimized next, so they are the most useful thing an owner of an older PC can send.

### What should I change first on a weak PC?

1. **Video > Graphics preset > Potato.** It lowers draw distance and shadows below the console's.
2. **Frame rate limit at 30.** A steady 30 fps feels better than a jumping 45.
3. **Lower the internal resolution, with FSR 1.**

If the resolution makes no difference, your CPU or driver is what limits you, and it is worth telling us on #589.

### What is the slowest setting?

Shadow resolution at Extreme, by far: in forests it can cut the frame rate to a third. After that, try lowering the
draw distance.

### What hardware is the minimum?

A GPU with a Vulkan 1.3 driver, a CPU with SSE4.2 and 16 GB of RAM. Without AVX2 and FMA the game code runs in a slower
compatibility mode. GPUs with only Vulkan 1.2, such as the GT 730 and the oldest Radeons on Windows, aren't supported.

### Is this faster than an emulator?

It depends on your PC, and we have no finished comparison to quote. Your CPU no longer translates the game while it
plays, but all the graphics work still has to be done, and the Vulkan drivers matter a lot.

### Things slow down at random or when I use a rune

Short drops right after a loading screen or when a rune starts were reduced in 0.5.1 by waiting less on the GPU
([#639](https://github.com/Revivedx/SavageWindRecomp/issues/639),
[#672](https://github.com/Revivedx/SavageWindRecomp/issues/672)). If you still see them on the latest version, report
how long they last and attach the log.

### It uses a lot of RAM

We are looking at reports of high use. Please post your log on the issue ([#525](https://github.com/Revivedx/SavageWindRecomp/issues/525)).

### Something behaves wrong above 30 or 60 fps

The game was made to run at 30 fps, and Savage Wind corrects each system that assumes that as it's found. Some still
break: physics puzzles, doors, platforms or scripted scenes. Tell us which one on
**[#425](https://github.com/Revivedx/SavageWindRecomp/issues/425)**, with a save from just before it. Going down to 30
fps gets you past it.

---

## Controls

### Can I change the keys and buttons?

Yes, every action in **Options > Key bindings**. Default keys are in [Controls](controls.md).

### Which controllers work?

Any controller SDL knows. Button prompts follow the device you use, set in **Controls > Button icons**.

### Does gyro work?

Yes, with DualSense, DualShock 4, Switch controllers and the Steam Deck. On a controller without one, hold T and move
the mouse to tilt in the motion shrines. On Linux, other devices can use a DSU server.

### Does it work with mouse aiming?

Yes, the mouse turns the camera directly with no acceleration, and sensitivity is in **Options > Controls**.

### The mouse cursor escaped, or I can't click

Esc frees the cursor, and clicking inside the window takes it back.

---

## Known issues

Everything here is confirmed or under investigation. If you know more about one, add it on its issue instead of opening
a new one. The full list is on the [issue tracker](https://github.com/Revivedx/SavageWindRecomp/issues).

### Gameplay

| Problem | Workaround | Issue |
|---|---|---|
| Mechanics and scripted scenes that break above 30 fps | Lower the frame rate to 30 for that moment | [#425](https://github.com/Revivedx/SavageWindRecomp/issues/425) |
| Game Over text stays half revealed, mainly between 70 and 120 fps | Try limiting to 60 fps | [#482](https://github.com/Revivedx/SavageWindRecomp/issues/482), [#507](https://github.com/Revivedx/SavageWindRecomp/issues/507) |
| Can't dismount the horse (seen at 120 fps; not reproduced yet, a save on the horse would help) | Lower the frame rate | [#625](https://github.com/Revivedx/SavageWindRecomp/issues/625) |
| Infinite loading screen after boarding a Divine Beast | Set the game up with the DLC in its own folders, not merged into `romfs` ([how](game-files.md#the-dlc)) | [#663](https://github.com/Revivedx/SavageWindRecomp/issues/663) |

### Graphics

| Problem | Seen on | Issue |
|---|---|---|
| Black, dark or wrong sky, can change between starts | AMD on Windows, RX 6000 and older | [#359](https://github.com/Revivedx/SavageWindRecomp/issues/359), [#665](https://github.com/Revivedx/SavageWindRecomp/issues/665) |
| Fog glitches in the intro | Under investigation | [#630](https://github.com/Revivedx/SavageWindRecomp/issues/630) |
| Transparent water | Under investigation | [#649](https://github.com/Revivedx/SavageWindRecomp/issues/649) |
| Graphical bugs | ROG Ally Z1E on SteamOS | [#614](https://github.com/Revivedx/SavageWindRecomp/issues/614) |
| Corrupted footprints in sand | Under investigation | [#634](https://github.com/Revivedx/SavageWindRecomp/issues/634) |
| Corrupted picture | Under investigation | [#632](https://github.com/Revivedx/SavageWindRecomp/issues/632) |
| Flicker on distant surfaces with fast camera (not reproduced yet) | Under investigation | [#105](https://github.com/Revivedx/SavageWindRecomp/issues/105) |

### Crashes and memory

| Problem | Workaround | Issue |
|---|---|---|
| Crashes on Intel integrated graphics and Arc | `sparse_heap = off` in `savagewind.ini` | [#619](https://github.com/Revivedx/SavageWindRecomp/issues/619), [#596](https://github.com/Revivedx/SavageWindRecomp/issues/596), [#594](https://github.com/Revivedx/SavageWindRecomp/issues/594) |
| Crash at startup on older Intel UHD graphics | Testers needed, attach your log | [#622](https://github.com/Revivedx/SavageWindRecomp/issues/622), [#589](https://github.com/Revivedx/SavageWindRecomp/issues/589) |
| Crash after about two hours of play, memory grows | Restart the game now and then | [#640](https://github.com/Revivedx/SavageWindRecomp/issues/640) |
| High RAM use | None yet | [#525](https://github.com/Revivedx/SavageWindRecomp/issues/525) |

### Performance

| Problem | Workaround | Issue |
|---|---|---|
| Older GPUs and integrated graphics | Potato preset, and report on the tracker | [#589](https://github.com/Revivedx/SavageWindRecomp/issues/589) |
| Frame generation slower instead of faster on some systems | Turn it off | [#635](https://github.com/Revivedx/SavageWindRecomp/issues/635) |
| TAA is expensive | Use another anti-aliasing option | [#654](https://github.com/Revivedx/SavageWindRecomp/issues/654) |
| FSR 3 and FSR 4 upscaling held back | FSR 1; redesign planned for Alpha 6 | [#597](https://github.com/Revivedx/SavageWindRecomp/issues/597) |

### Input and interface

| Problem | Workaround | Issue |
|---|---|---|
| Attack key follows the Sort / Sensor binding | None yet | [#608](https://github.com/Revivedx/SavageWindRecomp/issues/608) |
| Map controls with mouse and keyboard | None yet | [#440](https://github.com/Revivedx/SavageWindRecomp/issues/440) |
| Button prompts and layout on DualSense | None yet | [#604](https://github.com/Revivedx/SavageWindRecomp/issues/604) |
| Gyro axes wrong on some handhelds over DSU | Being looked at | [#136](https://github.com/Revivedx/SavageWindRecomp/issues/136) |
| HUD shakes or goes out of bounds at 20:9 (2340x1080) | Needs more information; a 16:9 window meanwhile | [#728](https://github.com/Revivedx/SavageWindRecomp/issues/728) |
| Gaps in Thai subtitles | Set Voices to "Same as text" | [#516](https://github.com/Revivedx/SavageWindRecomp/issues/516) |

### Setup

| Problem | Workaround | Issue |
|---|---|---|
| "No game files found" when reusing an install | [See above](#setup-says-no-game-files-found-in-that-folder) | [#590](https://github.com/Revivedx/SavageWindRecomp/issues/590) |
| Setup stops at the start of the build | [See above](#setup-stops-at-5-10-or-the-game-code-didnt-build) | [#618](https://github.com/Revivedx/SavageWindRecomp/issues/618), [#669](https://github.com/Revivedx/SavageWindRecomp/issues/669) |

---

## Reporting a problem

1. **Use the [latest release](https://github.com/Revivedx/SavageWindRecomp/releases/latest)** and game version 1.9.0.
2. **Search the [existing issues](https://github.com/Revivedx/SavageWindRecomp/issues?q=is%3Aissue)** first.
3. **[Open a new issue](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose)** and attach:
   - `savagewind.log`. On Windows it's next to `savagewind.exe`, on Linux in `~/.local/share/savagewind/`. A new one is
     written at each start, so take it right after the bug.
   - For a crash or a cutscene bug, your `save` folder, zipped, after closing the game.

**Make a problem report**, in Savage Wind Configuration, gathers these into one zip for you.

Where each kind of report goes:

| What | Where |
|---|---|
| Poor performance or broken graphics on an older or integrated GPU | [#589](https://github.com/Revivedx/SavageWindRecomp/issues/589) |
| Something that breaks at high frame rates | [#425](https://github.com/Revivedx/SavageWindRecomp/issues/425) |
| Anything else | [A new issue](https://github.com/Revivedx/SavageWindRecomp/issues/new/choose) |
| Code, a patch or a fix | [A pull request against `test`](../CONTRIBUTING.md), not an issue |

Before opening one, read this page. An issue about something it already covers, including a new report of a
[known issue](#known-issues), is closed right away with the reason and a link to the answer. If what it says didn't help
on the latest release, say so in your report and attach the log, and it stays open. A closed one is reopened if you
comment with that.

Reports missing the log get a `needs-info` label and are closed after 7 days without it. Never attach or link game
files, keys or firmware.

---

## Other settings

### Choosing the GPU

Close Savage Wind and edit `savagewind.ini`, in the same data folder as `savagewind.log`. Set `gpu = 1`, for example,
using the number printed beside your preferred adapter in the log's `[vk] gpu 0: ...`, `[vk] gpu 1: ...` lines, then
restart. `gpu = auto` (the default) prefers a discrete GPU. An unavailable adapter falls back to automatic selection
with a warning in the log. Adapter numbers can change after hardware or driver changes, so check the log again if
necessary. The `SAVAGEWIND_GPU` environment variable overrides this setting for one launch (an index or `auto`). This
selects the rendering GPU. The `display` setting selects the monitor.

### We asked you to try a test setting

Use the Launch options list on the start screen, so you don't need a terminal:
[launch-options.md](launch-options.md).

### What is `vk_pipelines.seed`?

A list of shader hashes and draw states met in a full playthrough, next to the executable (about 4,000 pipelines, 100
KB; it holds nothing of the game itself). The shader precompilation reads it so that the first boot already has every
pipeline that playthrough needed. Deleting it only costs some stutter on the first boots.

### Can I change the picture on the start screen?

Yes. Put your own next to `savagewind.exe` (on Linux, in the data folder) as `splash.jpg` or `splash.png`. It's only
read from your folder and is never part of Savage Wind.

### What are the F keys for?

F1 opens the overlay (with Cheats and more). F10 cycles the performance overlay. F12, or the key below Esc, opens the
[console](CONTROL.md). All the keys: [Controls](controls.md).

---

## How do I get the game files?

**We won't tell you, and nobody here will.**

Not how to dump a cartridge, not which tool to use, not where to get keys or firmware, not which guide to follow, and
not "just a hint in private". Questions like this in issues get removed, and accounts that keep asking are blocked.

That isn't unfriendliness, and there are good reasons for it:

- **The law.** Getting the game out of a console means getting past its technical protection. Many countries restrict
  circumvention and helping others do it, separately from whether you own the game. What is allowed differs from one
  country to another. We aren't lawyers, and we can't tell you what's legal where you live.
- **It would put the project at risk.** Savage Wind can exist because it's only code of its own, and because everything
  of Nintendo's comes from the player. Instructions for circumventing protection, or links to tools, are the kind of
  thing that gets projects like this taken down, and then nobody has it.
- **We can't tell who owns what.** A guide written for owners helps anyone at all, and the project doesn't support piracy
  in any form.
- **It has to stay clean.** For the same reason the repository has no game files, no keys and no generated game code,
  and Savage Wind doesn't include anything that decrypts.

**What you need to know:** Savage Wind needs your own copy, version 1.9.0, from your own console, already extracted to
`exefs/` and `romfs/` folders. Once you have that, we are glad to help with anything that goes wrong
([Game files](game-files.md)). What happens before that point is up to you.

**And "can you send me the files?"** No. Don't ask anyone here, and don't post links. See also
[Respect copyright](../README.md#respect-copyright).

---

## The recompilation

### What is a static recompilation?

An emulator translates a game's machine code while the game runs. A static recompiler does it once, ahead of time: it
reads the Switch executables, turns every function into C code and compiles that into a program for your PC. When you
play, your CPU runs native code.

### Is this an emulator?

No. There's no Switch being imitated while the game code runs. The game's code runs directly, and Savage Wind replaces
what is around it: the operating system services, the graphics API, audio and input. It keeps one thing an emulator also
needs, a translator for the game's GPU shaders.

### What does Savage Wind actually do at setup?

1. It checks your files are version 1.9.0.
2. It copies them to its own data folder.
3. It recompiles the executables into C on your machine, with a bundled compiler, and builds them into a game library.
4. It translates and precompiles the shaders for your graphics card.

### Why is the compile done on my PC and not shipped?

Because the result contains Nintendo's code. It's generated on your machine from your files, it's for your own use, and
you must not share it. Our releases only contain Savage Wind's own code.

### How does it draw the game?

The game talks to NVN, the Switch's graphics API. Savage Wind implements NVN on top of Vulkan, and has its own translator
from the Switch's Maxwell GPU shaders to Vulkan. That layer is where most of the graphics bugs live, and it's why a
problem can be specific to one vendor or one driver.

### Is it a decompilation? Does it use the decompilation projects?

No. The recompiler translates machine code automatically, and nothing is written from a decompilation project. Code from
those projects isn't accepted (see [CONTRIBUTING](../CONTRIBUTING.md)). The game's own behaviour is studied in our own
copy of the game.

### Why only one version of the game?

The recompiler, and the patches that add higher frame rates, wider screens, the options menu and more, are tied to exact
addresses in one build. Every other version would be close to a separate project to maintain. Version 1.9.0 is the
latest one, and is what you have after updating. For this reason, older versions and other sources of the game such as
the Wii U version won't be supported.

### Why do some glitches work differently?

They depend on the game version and sometimes on the frame rate. Savage Wind is 1.9.0, so what the official updates
removed stays removed, such as some windbomb variants
([#508](https://github.com/Revivedx/SavageWindRecomp/issues/508)). There's a
[glitch compatibility tracker](glitch-compatibility.md) to record results.

### Why are there still bugs if the code is the original?

Because everything around it is new: graphics, OS services, timing. A real Switch does some things in an order or a time
that the game has silently come to depend on. High frame rates and huge resolutions are also things the game was never
written for.

### Can I use mods?

Partly, and not yet the way you would on Cemu. Language packs work: `lang/<language>.txt` in the data folder is read over
the built-in translations. A loader for other mods is planned, and because this isn't an emulator, Wii U and Cemu
packs need adapting. Mods must not ship game files
([#647](https://github.com/Revivedx/SavageWindRecomp/issues/647),
[#645](https://github.com/Revivedx/SavageWindRecomp/issues/645)).

### Does it support amiibo?

There's a virtual reader for your own figure dumps, in the overlay (F1 > amiibo). Savage Wind doesn't include any figure
data, and won't. Getting amiibo rewards without figures is planned.

### What is built on what?

Savage Wind is built on [LAS Recomp](https://github.com/peiikai/LASRecomp), with
[TotK Recomp](https://github.com/valentderah/totk-pc-recomp) and [Ryujinx](https://github.com/Ryujinx/Ryujinx) as
references. Details: [Credits](third-party.md).

---

## Platforms and requests

### Windows and Linux?

Both are maintained, on x86-64. On Linux use the AppImage ([linux.md](linux.md)).

### Steam Deck and handhelds?

The Steam Deck works and is the handheld that gets tested. Other PC handhelds are recognized and have a preset, but only
the Deck has been tried by us.

### Wine or Proton?

Not supported. The AppImage exists so that Linux doesn't need them
([#366](https://github.com/Revivedx/SavageWindRecomp/issues/366)).

### macOS?

Not planned as a target. The Metal code inherited from LAS Recomp is still there, but nobody maintains it.

### Linux on ARM? Android?

The ARM AppImage is highly experimental. Android is outside the project's scope, though a good pull request would be looked at
([#433](https://github.com/Revivedx/SavageWindRecomp/issues/433)).

### Tears of the Kingdom?

No plans. Savage Wind is only this game, at least until it ends its Beta phase. There are separate projects for that game.

### Multiplayer?

No. It's an open request ([#617](https://github.com/Revivedx/SavageWindRecomp/issues/617)), but nothing like it is in the
game.

### What's planned next?

The [Status](STATUS.md) page shows every part, how far it is and what's planned. Alpha 6 has the motion vector redesign
for FSR and TAA as its main job.

---

## Philosophy

### Why does this project exist?

To give a game that deserves it a real PC version, on real PC terms: native code, any frame rate, any resolution, proper
keyboard and mouse. And to make sure nobody with an old PC is left out, which is why Alpha 5 was the "Potato" update.

### Why is it made with AI?

Because that's how a project this size can be made by one person. Nearly all of the code, tooling and reverse engineering
is written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by a human. We say so up
front, because people have a right to decide with that information. If you don't trust code written by AI, you can
simply ignore this project.

### Why is it free? Can I donate?

It's free, and there's no way to donate, on purpose. Nintendo still sells this game, and a fan project that takes money is
a different thing, legally and morally, from one that doesn't. There are no paid versions, no early access and no
sponsors. The best way to help is a good bug report.

### Why do I have to own the game?

Because it's the only honest way to do this. Savage Wind is a new way to play a game you already have, not a replacement
for buying it. If you like what it does, Nintendo made the game, and they deserve the sale.

### What does the project refuse to do?

- Ship, host or link game files, keys, firmware or translated game text.
- Help anyone circumvent anything.
- Take money.
- Add its cheats into the game's own menus. They stay in the overlay and the console, so the game looks as it should.

### How is the Status number calculated?

It's a weighted average, and how it's weighed is on the [Status](STATUS.md) page. It is an estimate of how far the project
has come against what it set out to do, not a promise.

### How can I help?

- **Report bugs well.** A log and a save are worth more than a long description.
- **Test on hardware we don't have**, especially older GPUs, Intel and AMD on Windows.
- **Write code:** read [CONTRIBUTING](../CONTRIBUTING.md) first. Open pull requests against `test`, keep each one to one
  change, and say how you tested it. Every pull request is tested by the maintainer before it goes in, and its author is
  credited.

### Why do you close issues so fast?

Because one person reads all of them. Anything this page already answers is closed at once with the reason, duplicates
point to one place, reports without a log can't be acted on, and every closed issue can be reopened by commenting with
new information.

---

## Who is behind this?

> Arriba el Perú, carajo.

Savage Wind is maintained by **ReviveDx** ([GitHub](https://github.com/Revivedx)), who directs the project and reviews
and tests what goes into it, with the help of its
[contributors](https://github.com/Revivedx/SavageWindRecomp/graphs/contributors) and everyone who has sent a
useful report.

**Discord:** `@ReviveDx` · **Website:** [revi.contact](https://revi.contact)

If you want to contribute, you can contact me there to coordinate with the rest of the contributors. Please don't use
either for bug reports or support, and never to ask for game files: bugs go on [GitHub](#reporting-a-problem), where
other people can find the answer.
