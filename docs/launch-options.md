# Launch options

Sometimes we ask you to test with an extra setting turned on, for example "start the game with `SAVAGEWIND_FSR3`
set to 1". These settings are called launch options (programmers call them environment variables). You do not need
a terminal or a batch file: you can set them on the Savage Wind start screen.

## Add a launch option

1. Open the start screen (Savage Wind Configuration) and stay on the first page, the one with Game files and Game
   font. Scroll down to **Launch options**. The list is empty until you add something.
2. Click the first field and type the name, for example `FSR3`. You can leave out the `SAVAGEWIND_` start. While you
   type, matching suggestions appear below the fields; click one to fill in its name and an example value. You can
   also type any name we gave you that is not in the suggestions.
3. Click the second field and type the value we asked for, for example `1`. If you leave it empty, `1` is used.
4. Press Enter, or click **Add**. The option shows up in the list. With a controller, press A on a field to get the
   on-screen keyboard.
5. Press **Play**. Savage Wind restarts once by itself and then starts the game with your options. You may see the
   window close and open again; that is expected. Starting the game directly with `savagewind.exe` (or the AppImage)
   uses your launch options too, with the same quick restart.

Launch options stay until you remove them, also after you close and reopen Savage Wind.

## Change or remove one

- To change the value, edit the value field in the list and press Enter.
- To change the name, remove the option and add it again.
- To remove an option, click **Remove** next to it. The next time you press Play it is gone.

## Switches are on or off, not 0 and 1

Many options are plain switches: they are on when they are in the list and off when they are not. For those,
writing `0` does **not** turn them off (the game only checks that the option exists). To turn a switch off, remove
it from the list. Savage Wind refuses `0`, `off`, `false` and `no` for the switches it knows about, and shows a
message instead.

## Warnings

- Only add options you were asked to add. A wrong option can make the game slower, glitchy or crash at start.
- Options shown in orange are for developers. Some of them make the game much slower or write very large files.
- Savage Wind does not accept options that would lock you out of this screen or break the start (for example
  `SAVAGEWIND_NO_LAUNCHER`), or system settings such as `PATH`.
- If the same variable is also set outside Savage Wind (in a shortcut or a terminal), the value in this list wins.

## If something goes wrong

The start screen opened with `--config` (what Savage Wind Configuration does) always starts **without** your
launch options, so you can always get back in and remove a bad one.

To start the game ignoring all saved launch options once, start it with `--no-launch-flags`, for example
`savagewind.exe --no-launch-flags`.

To clear them completely, close Savage Wind and delete the file `launch-flags.txt` in the data folder (the same
folder as `savagewind.ini`; the log line `[paths] data: ...` shows where it is).

## For testers who like details

- The options are kept in the text file `launch-flags.txt` in the data folder, one `NAME=value` per line. Lines
  starting with `#` are comments. It can be edited by hand while Savage Wind is closed; lines that are not valid
  are ignored.
- The names of the suggestions come from the program itself: `tools/gen_launch_flags.py` collects every
  `SAVAGEWIND_` name that the code reads into `runtime/launch_flags_table.c`, and `make test-launch-flags` fails
  when that list is out of date. The short descriptions of the suggested options are in
  `tools/launch_flags_curated.txt`.
- A name you type that is not in the list still works; it is passed to the game as it is.
