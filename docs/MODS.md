# Mods

Savage Wind plays BNP mods merged by [BCML](https://github.com/NiceneNerd/BCML). Your game files in `extracted/` never
change: the merged result goes to `mods/.merged/` and the game reads it over them.

## Adding and removing

The folder is the list: what is in `mods/` is what you play with.

1. Close the game.
2. Add a mod by putting its `.bnp`, `.zip`, `.rar` or `.7z` in the `mods/` folder of your data folder. Remove one by
   deleting that file.
3. Start the game. If `mods/` changed, a window shows the mods being applied first (a minute or two), then the game
   starts. If nothing changed, nothing runs.

No Python is needed.

## Turning mods on and off

Options > Mods has a switch for each mod (and so does the Language tab of Savage Wind Configuration). Turning a mod off
moves its file to `mods/disabled/`, and turning it on moves it back; you can also move the files by hand. It applies the
next time the game starts: the page's Restart now does it at once. Mods added while the game runs appear after a restart.

To play without any mods for a while, set `SAVAGEWIND_NO_MODS=1`.

A mod's optional parts are installed as its author set them by default.

## What works

- An archive can hold BNPs (each one is installed), a mod with its `info.json`, or a Switch mod's `romfs` folder, also as
  `01007EF00011E000/romfs` or `atmosphere/contents/01007EF00011E000/romfs`, or the game's folders (`Actor`, `Pack`, ...)
  loose in a folder. A mod that fails doesn't stop the others:
  it is reported in `mods/.bcml/errors.txt` and tried again the next time the game starts.
- `rules.txt` mods are Cemu graphic packs, for Wii U: they don't work.
- `Layout/Common` in `Pack/Bootup.pack` always stays the game's own. Old mods bring their whole `Bootup.pack`, and the game
  crashes on the layout of another version, so a mod that changes the HUD or menus that way has no effect.
- BNPs for Switch. BCML targets version 1.6.0 and Savage Wind plays 1.9.0, so a mod made for 1.6.0 can work or not.
- Mods for Wii U need converting first, and Cemu code patches don't apply.

## How it works

`runtime/mods.c` compares `mods/*.bnp` with what was last applied when the game starts. If they differ it runs
`swmods`, which is `tools/mods.py` and BCML frozen with PyInstaller (`make mods-helper`, `tools/swmods.spec`). It installs
and uninstalls the difference, merges, and writes `mods/.merged/`. If that folder exists the game uses it as its RomFS
overlay (`overlay_dir` in `runtime/services/fs.c`). `SAVAGEWIND_ROMFS_OVERLAY` still takes priority over it.

Automatic applying is Windows only for now. Elsewhere, or in a source checkout without the helper, run
`python3 tools/mods.py` (it installs BCML in `mods/.bcml/venv` on its first run; `tools/mods.py list` shows what is
installed).

BCML is GPLv3 and Savage Wind is MIT, so BCML stays a separate program: `swmods` is its own executable, started as a
process. A release that ships `swmods` has to follow the GPL for it, with its licence and the source of what it contains.

A merged `ResourceSizeTable` is grown as the game's own is (`rstb_patch`): the game can't load the files Savage Wind
makes bigger, such as `Layout/Common.blarc`, with the sizes BCML wrote.
