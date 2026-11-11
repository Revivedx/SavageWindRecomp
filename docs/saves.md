# Saves

[Back to the README](../README.md)

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

Want more than one set of saves, for example your own playthrough and a 100% save for testing? **Settings > Saves**
keeps several profiles and switches between them. See [Save profiles](save-profiles.md).

Played on the Wii U or in Cemu? Savage Wind Configuration can import that save: open **Settings > Saves** and choose
**Import from Wii U / Cemu...**. It backs up your current saves first and asks before it replaces them. See
[Importing Wii U / Cemu saves](save-converter.md).
