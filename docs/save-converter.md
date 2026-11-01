# Importing Wii U / Cemu saves

Savage Wind Configuration can import a *Breath of the Wild* save from the Wii U, or one played in Cemu, and turn it into
a save Savage Wind can load. You do not need Python or a terminal. It reads only your own files and ships no game data.

## Use it

1. Close the game and open **Savage Wind Configuration**. Go to **Settings > Saves**. The tab is only on the start
   screen, not in the in-game menu, because the game must not be running while its saves change.
2. Choose **Import from Wii U / Cemu...** and pick the Wii U profile folder. In Cemu it is
   `mlc01/usr/save/00050000/<title id>/user/80000001`, the folder with the numbered slots `0`, `1`, ... and
   `option.sav`. A folder above it works too: the importer looks up to ten levels down and prefers a Wii U save to
   anything else it finds. The search stops after four seconds or 100,000 folders.
3. The save is converted into a new folder next to your `save` folder. Your saves are not touched yet. When that is done,
   a question names the backup folder and shows which slots come in. Choose **Import**, or **Cancel** to leave
   everything as it is (the converted copy is removed again).
4. Your current `save` folder is moved whole to `save-backups/save-<date>-<time>`, next to `save`. The converted save
   takes its place, and whatever else the old folder held besides `save_0` (the `sdcard` folder) is copied back.
   Nothing is deleted.
5. The tab shows what was imported: the slots, how many flags were kept, set to their defaults or dropped, and how
   many Hero's Path blocks, album pictures and compendium pictures came along. The slot list below it is read again.

The import replaces the whole of `save_0` (all slots, `option.sav`, the Hero's Path, the album and the compendium
pictures), not just some files in it. Mixing slots of two games would give a Hero's Path that does not match the save.
To go back to what you had: close the game, rename `save` to something else, and rename the backup folder to `save`.

The other two buttons: **Back up saves** copies the whole `save` folder to a new dated folder in `save-backups`
and leaves the saves where they are, and **Open save folder** opens it in your file manager.

The slot list shows each slot's picture (`caption.jpg`, on the right while the row is selected), when the game wrote the
slot (the file time of its `caption.sav`) and the play time, if the save's `caption.sav` has a `PlayTime` flag. The
importer keeps the file times of the Wii U files, so an imported slot shows the date it was saved on the Wii U.

It needs your game files set up: the importer reads the flag tables from `Pack/Bootup.pack` in them (see below).

## When it does not work

Each of these ends with "Nothing was changed.", and your saves are exactly as they were:

- **No Breath of the Wild save found in that folder.** Pick the folder with the numbered slots, or one above it.
- **The search took too long. Choose a folder closer to the save.** The time or folder limit was reached.
- **That is a Switch save already.** Switch saves are used as they are: copy their files into the save folder instead.
- **A save file is damaged or incomplete.** The message names the file. A half-copied save lands here.
- **The game files weren't found / can't be read.** Set up the game files on the start screen first.
- **Savage Wind is running in a game.** Another Savage Wind has the save folder open (see below). Close the game first,
  then try again.
- **The save folder is a link.** Moving a link would move the link, not the saves, so the importer stops.
- **Files could not be read or written.** Close anything that has the save folder open (a file manager window, a
  backup program) and try again.

If the last step fails after the old folder has been moved, the move is undone. In the rare case that cannot be undone
either, the message says "The import stopped halfway" and names the backup folder: rename it to `save` to get your
saves back.

A running game holds a lock on its save folder: the empty file `save.lock` next to `save` (not inside it, so the swap
never moves it). The operating system drops the lock when the game ends, however it ends, so a crash leaves nothing to
clean up; the file itself stays. While another Savage Wind holds it, the tab refuses to import or to back up, because
the running game could save over an imported slot, or a backup could copy a file that is half written. Choosing a
folder to import from is refused straight away, before anything is converted. The game does not wait for the lock: a
second game started on the same save folder still starts, and logs that the lock is taken. The tab also holds the lock
while it swaps or copies, so two Savage Winds cannot do that at once. If the lock file cannot be made (a read-only
folder), nothing is locked and nothing is refused.

Closing the start screen or quitting waits for its save worker. A conversion can stop between files and removes its
staged copy. Once installation starts, quitting waits for the two moves and the carry-over copy to finish; it does not
cancel an installation between moves. This covers normal Quit and window close, not a crash, power failure or forced
process termination. A blocked operating-system file read or move can delay quitting until it returns.

When the tab opens, it removes abandoned dated `save-import-*` folders beside `save`, leaving a live or unconfirmed
stage alone. If `save` is missing, it keeps every import folder and shows a recovery message naming the newest import
and dated backup folders. Close Savage Wind and rename the desired folder to `save` to recover it. Imports and backups
are disabled while this recovery is needed. **Open save folder** opens the preserved import without creating an empty
`save` folder. Cleanup does not follow symbolic links or Windows junctions.

## What it does

A save is a handful of small files per slot. `game_data.sav` holds every game flag, `caption.sav` the metadata the
load screen shows, `caption.jpg` its picture; `option.sav` holds the settings, `tracker/` the Hero's Path, and `album/` and `pict_book/` the photos.

The flag files are the same on both consoles apart from three things, and the importer handles each:

- **Byte order.** The Wii U is big endian, the Switch little endian. Every 32-bit value is swapped. Text flags are
  stored as plain bytes and are not swapped.
- **Revision.** The first word of the file is the game revision (`0x471B` for the Wii U's 1.5.0, `0x4730` for
  Switch 1.9.0). Flags are matched by the CRC32 of their name, so a flag that only exists in the target revision gets
  its default value, and one that no longer exists is dropped. The report counts both.
- **Types and sizes.** Which flags exist, their types, array lengths and defaults are read from the game's own
  `GameData/savedataformat` and `gamedata` tables inside `Pack/Bootup.pack`. Nothing is hard coded, so another game
  revision works with its own `Bootup.pack`.

`option.sav` is converted the same way, but the two consoles have different options, so settings that do not exist
on both sides take the Switch default.

`tracker/` (the Hero's Path) is byte-swapped in 32-bit words, except its 8 byte header, which holds four plain bytes
and two 16-bit fields. `album/` and `pict_book/` are plain JPEG files and are copied.

Slots keep their number. The game decides which slot "Continue" loads.

The code is in `runtime/services/save_convert.c` (the importer, with no SDL in it), `runtime/services/archive.c` (the
Yaz0 and SARC readers it shares with `fs.c`) and `runtime/ui/saves_tab.mm` (the tab). The conversion runs on a worker
thread. The slot list is also read on that worker, with a loading message while it is being read, so the start
screen is not held up while it reads the game's tables and writes the files.

## The Python tool

`tools/botw_save_convert.py` does the same conversion from a terminal. It is the reference the importer follows and the
test oracle for it, and it can do two things the tab does not offer: convert a Switch save back to the Wii U layout
(`--to wiiu`) and choose slots (`--slots 0,2`) or leave the Hero's Path out (`--tracker skip`). It needs Python 3 and
nothing else.

```
python tools/botw_save_convert.py --src <Cemu profile folder> --dst save/save_0
```

`--bootup` picks another `Pack/Bootup.pack` (default `extracted/romfs/Pack/Bootup.pack`) and `--dry-run` converts in
memory and only reports. The tool never overwrites: files that already exist in the destination are skipped, and
`--overwrite` renames them to `<name>.bak-<time>` first. It does not make backups of the whole folder; the tab does.

## Tests

`python tools/test_botw_save_convert.py` runs the tests of the Python tool, which build their flag table by hand and
need no game files.

`make test-save-convert` (or `python3 tools/test_save_convert.py`) builds the importer into a test program and compares it
with the Python tool: both convert synthetic saves made against a small `Bootup.pack` built by hand, and every output
file has to match byte for byte, along with the per-file flag counts. It also covers the refusals (no save, a Switch
save, damaged saves, a missing or broken `Bootup.pack`), folders with non-ASCII names, and the safety of the backup,
the swap and its rollback. No game files are needed.

The script also runs the Saves tab itself without a window (`tools/test_saves_tab.cpp`: Dear ImGui builds the frames in
memory, the importer runs on its worker thread, and the question is answered with a click) on copies of a synthetic save
folder, and checks the files on disk afterwards: import over an existing save, a first import, Cancel, the refusals and
Back up saves, and an import and a backup refused while another process holds the save folder's lock (a second test
process stands in for the game, also killed to check that its lock goes with it). It also quits during conversion, at confirmation, and between the two installation moves, and checks
recovery after an interrupted installation. The C cases cover cancellation, stale cleanup and keeping live stages or
all imported copies when `save` is missing. That part needs a C++ compiler and SDL3's headers; set
`SDL3_INCLUDE=<folder with SDL3/SDL.h>` if they are not on the default path (without them it says SKIP), and `--no-ui` leaves it out. It draws no pixels and uses no real
window, mouse or controller, so it does not replace looking at the tab.

With your own files, the same script compares both implementations on your real save:

```
python3 tools/test_save_convert.py --src <Cemu profile folder> --bootup <Pack/Bootup.pack>
```

Nothing is written outside a temporary folder that is removed afterwards.

Against real files: converting a Switch save for Switch reproduces it byte for byte when it is already the target
revision. A published 100% save exists in both a Wii U and a Switch version. Converting the Wii U one to Switch
gives the same 349,508 of 349,538 flags (8 slots plus options) as converting the Switch one for the same game files; the other 30 are text
flags, which that Switch file stores word-swapped (a native 1.9.0 save keeps text as plain bytes, so the tool does too).
All 36 Hero's Path files, and the album and compendium pictures, come out identical to the other console's copy in both directions. A Switch save converted to the Wii U layout also loaded in Cemu without problems.
