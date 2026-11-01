# Moving the data folder

Set `SAVAGEWIND_DATA_ROOT` to choose where Savage Wind keeps its game files, saves, settings and caches. This
already works on Windows and Linux; it is an environment variable, not an option in the configuration screen.
Use an absolute path to a writable folder. The destination drive must be available whenever you start the app.

## Copy your existing data

1. Close the game and Savage Wind Configuration.
2. Find the current data folder. The startup log prints it as `[paths] data: ...`. By default it is the folder of
   `savagewind.exe` on Windows, or `~/.local/share/savagewind` on Linux (`$XDG_DATA_HOME/savagewind` if
   `XDG_DATA_HOME` is set to an absolute path). Source builds started in a folder containing `extracted/` use that
   starting folder instead. An existing `SAVAGEWIND_DATA_ROOT` takes precedence over these defaults.
3. Copy your data into an empty destination folder, keeping the same structure: `extracted/`, `save/`, `cache/`,
   `fonts/` and `savagewind.ini`, where present, along with any custom files such as `splash.png`.
   The destination should contain `extracted/` and `save/` directly, not another enclosing copy of the old folder.
   Copy the whole `save/` folder, including `save_0` and its contents.
4. Start Savage Wind with the variable set, using one of the examples below. Replace the example paths with your
   own application and destination paths.
5. Check `[paths] data: ...` in the new startup log, then check your settings and load your existing save.
   Keep the old data until you have verified the copy. Afterwards, you can remove the old data you copied to free
   space; keep the original application, its DLLs and helper tools in place.

The variable does not move or import anything by itself. An empty destination starts with no saves or settings
and asks you to set up the game files. Your original data stays in the old folder.
If you also set `SAVAGEWIND_SAVE_DIR` for testing, it overrides the save location separately: unset it or update it
when moving the whole data folder.

## Windows

For one PowerShell session:

```powershell
$env:SAVAGEWIND_DATA_ROOT = 'D:\Games\Savage Wind Data'
& 'C:\Games\Savage Wind\savagewind.exe'
```

To use the same folder each time, save this as a `.cmd` file and launch it instead of the executable:

```bat
@echo off
set "SAVAGEWIND_DATA_ROOT=D:\Games\Savage Wind Data"
"C:\Games\Savage Wind\savagewind.exe" %*
```

Both examples leave the executable, DLLs, `zig/` and `swgen/` in the application folder. Add `--config` when
launching to open Savage Wind Configuration with the same data folder. The variable in a PowerShell session or
`.cmd` file does not automatically apply when you launch the executable another way.

## Linux / Steam Deck

For one launch:

```sh
SAVAGEWIND_DATA_ROOT="/mnt/games/Savage Wind Data" "/path/to/Savage_Wind.AppImage"
```

For repeated launches, save this as `run-savagewind.sh` and make it executable with `chmod +x run-savagewind.sh`:

```sh
#!/bin/sh
export SAVAGEWIND_DATA_ROOT="/mnt/games/Savage Wind Data"
exec "/path/to/Savage_Wind.AppImage" "$@"
```

Run `./run-savagewind.sh --config` to open configuration with the same data folder. If you launch through a desktop
entry or Steam, point it at the wrapper or set the variable in that launcher's environment too. Launching the
original AppImage directly does not inherit the wrapper's variable. You can keep the AppImage in its original
location; it does not need to be copied into the data folder.

## Returning to the original folder

Launch without the override, or change the path in your wrapper. In a PowerShell session,
`Remove-Item Env:SAVAGEWIND_DATA_ROOT` unsets the variable; in a Linux shell, use `unset SAVAGEWIND_DATA_ROOT`.
This only changes which folder the next launch uses: saves and settings are not synchronized between folders.
If you want to keep newer progress when switching back, close the game and copy it back first, keeping a backup
of the destination's saves.
