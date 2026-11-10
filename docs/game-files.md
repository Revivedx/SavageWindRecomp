# Game files

[Back to the README](../README.md)

- Your own *Breath of the Wild* (cartridge or digital) and its **1.9.0 update**, the latest, extracted from your own
  console:
  - the **ExeFS** of that version (`main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`)
  - the **RomFS** with that update applied (base game and update merged)
- Optional: the DLC, *The Master Trials* and *The Champions' Ballad*, from your own copy: the **RomFS** of each of
  the two DLC, version 3.0 (see [The DLC](#the-dlc) below).
- Windows 10 or later or Linux, on x86-64 (a CPU with SSE4.2; without AVX2 / FMA the game code runs in a slower
  compatible mode), and a GPU with Vulkan 1.3.

Keep them in one folder like this one. The setup asks for it, and its name and place don't matter; a source build
reads `extracted/` next to the program (git ignores this folder):

```
extracted/
  exefs/   main, main.npdm, rtld, sdk, subsdk0
  romfs/   the complete RomFS (base game + the 1.9.0 update merged)
  aoc1/    optional: the first DLC's RomFS (11 folders, about 7,000 files, 1.4 GB)
    Game/  Map/  Movie/  NavMesh/  Pack/  Physics/  Sound/  System/  Terrain/  UI/  Voice/
  aoc2/    optional: the second DLC's RomFS (1 folder, 1 file)
    System/AocVersion.txt
```

The `main` module's build id tells the exact version; `python tools/sw_probe.py` (it reads `extracted/exefs`)
prints it and what the recompiler will find there.

## The DLC

Each DLC's RomFS goes in a folder of its own, named `aoc1/` and `aoc2/` as above, with its folders right inside it
(`aoc1/System/AocVersion.txt`). Keep the two DLC apart: the second one's RomFS is a single file, and extracting both
into one folder leaves only one DLC.

Do not merge the DLC into the game's `romfs/` either. A `romfs/` with the DLC inside (about 47,700 files instead of
41,062) lets the game start, but the loading screen after a Divine Beast boss then never ends
([#438](https://github.com/Revivedx/SavageWindRecomp/issues/438), [#570](https://github.com/Revivedx/SavageWindRecomp/issues/570)):
the same save loads on to the inside of the beast with a `romfs/` of 41,062 files and the DLC in `aoc1/` and `aoc2/`.
The game files page tells you when the DLC is merged.

- **With the game:** keep them in the same folder as `exefs/` and `romfs/`, and choose that folder at the first
  setup. The DLC is set up together with the game.
- **Later:** in Savage Wind Configuration's game files page, choose a folder that holds just the DLC folders and set
  up. The DLC is added to the game files already set up, without building the game code again.

The setup tells the game's RomFS (it has `System/Version.txt`) from a DLC's (`System/AocVersion.txt`) and copies the
DLC as `aoc1/` and `aoc2/` into the data folder. A folder the game reads in place (an `extracted/` folder next to the
program) is not copied, so there the DLC folders must already be named `aoc1/` and `aoc2/`. The game files page shows the DLC set
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
