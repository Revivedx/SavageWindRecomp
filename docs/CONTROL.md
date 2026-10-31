# Control commands

One list of named commands that more than one front end can run. Today that is a local socket, for scripts, AI agents
and `tools/swctl.py`, and the in-game console. Each command is either player safe (it only reads state or copies text, it cannot break the game)
or developer only.

## Who may run what

| | player safe commands | developer commands |
|---|---|---|
| normal start | yes | no |
| `SAVAGEWIND_DEV=1` | yes | yes |
| `SAVAGEWIND_CONTROL=<port>` | yes | yes |

A normal start has no socket and no developer commands: they are not listed by `help`, not run, and answer
`Unknown command`. `SAVAGEWIND_CONTROL=<port>` also opens the socket, `SAVAGEWIND_DEV=1` only unlocks the developer
commands in the in-game console.

The socket listens on `127.0.0.1` only (not reachable from the network). A connection whose first line is an HTTP
request (a web page in a browser on the same machine can send one to `127.0.0.1`) is closed before anything runs.

```
SAVAGEWIND_CONTROL=7777 ./savagewind        (Windows: set the variable, then start savagewind.exe)
python tools/swctl.py state
```

A command is player safe when its entry in the table has the `CONTROL_PLAYER` flag (`runtime/control.h`). Leave the flag
off for anything that writes memory or files, moves things or ends the game.

## The in-game console

The same commands can be typed in the game: press F12 or the key below Esc, left of 1.
The game gets no input while the console is open. The same key, F12 or Esc closes it. Up and Down walk through what you typed,
Tab completes a command, Ctrl+V pastes from the system clipboard, `clear` empties the screen. The console is in English,
like the command names and replies it shows.

It runs the same commands as the socket, with the same rights: a normal start offers only the player safe commands
below, with no socket. Text a command asks to copy (`pos`, `report`) goes to the system clipboard. `exit` closes the
game the way the window's close button does.

## Player safe commands

| command | what it does |
|---|---|
| `help` | the commands you can use |
| `version` | game version, operating system and architecture |
| `report` | one block for an issue: version, graphics card, processor, memory, frame rate, position and every setting (copied to the clipboard in the console) |
| `pos` | Link's position (x, y, z) and stamina, and `place`, a line like `swr:1 x=-1234.56 y=250.00 z=789.01 yaw=90.0 pitch=-10.0 hour=18.50`, to say where a bug is. Yaw and pitch work in any AA mode, the hour when known. An error before a save is loaded |
| `camera` | where the world camera is (`x y z`) and which way it looks (`dx dy dz`, unit vector; `yaw` and `pitch` in degrees: yaw 0 looks along +z and 90 along +x, pitch positive looks up). Read only, works in any AA mode: it uses the renderer's TAA view matrix when there is one, otherwise the game's stored camera. The stored camera reports its stored eye position, which can differ from the drawn one by about two metres; the direction is the same. An error before a world is loaded |
| `get [key]` | the current settings, or one; keys are `Settings` field names, plus `window_size`, `fps` (`fps_limit`), `left_stick_deadzone` and `right_stick_deadzone`. Enum values use lowercase names; values are the configured ones (environment overrides and hardware fallbacks still apply). Read only |
| `state` | frame number, uptime, fps over the last second and the hour of the day (left out before a world exists) |
| `exit` | end the game: the console closes the window the normal way, the socket exits at once |

## Developer commands

Only with `SAVAGEWIND_CONTROL` or `SAVAGEWIND_DEV=1`. Anything that writes memory, moves things or presses buttons is here.
Most of them rely on addresses found in game version 1.9.0 and are untested on other versions.

| command | what it does |
|---|---|
| `ping` | Check that the game answers (data: `pong` = true) |
| `time [freeze \| unfreeze \| set <0-24> \| <0-24>]` | Show the hour of the day. `set <hour>` or just `<hour>` sets it and keeps the clock running or frozen as it was. `freeze` holds the current hour, `unfreeze` continues from it at the normal speed. It writes both `WM_Time` buffers and lifts the early game 11:00 clock cap, without touching the paraglider quest flag. `SAVAGEWIND_TEST_TIME` still holds its configured hour |
| `shot <file>` | Save a screenshot as PNG, on the machine the game runs on (Vulkan only for now). The file must be a `.png` inside the game's folder: a relative path without `..` |
| `goto <x> <y> <z>` \| `goto <swr:1 line>` | Go to a place from `pos` or `report`: move Link, set the hour of the line (the clock keeps running, or stays frozen if it was) and turn the camera to its yaw and pitch like `look`. Same limits as `teleport` |
| `teleport <x> <y> <z>` \| `teleport status` \| `teleport forget` | Move Link. Writes the real position, reached through a pointer chain from `PlayerInfo` (found with `mem path`, checked against `pos`). If the chain does not match it falls back to a background search that never blocks a frame; poll `teleport status` (`scanning`, `probing`, `testing`, `verifying`, `done`, `failed`). Stand still when you start; do not use it while riding, in a cutscene or loading. A target below the ground makes Link fall. `forget` drops the saved address |
| `look <yaw> <pitch>` \| `look status` \| `look stop` | Turn the camera (degrees, either may be `keep`, pitch within -89..89). Writes the camera cells and checks again each frame, because the game moves the eye a little after a write. Without the cells it falls back to the right stick (needs the TAA camera read). Returns at once, poll `look status` (`running`, `done`, `timeout`, `failed`) |
| `fly [on \| off \| toggle \| mode look \| level \| speed <n>]` | Fly mode: Link floats and the controls steer him through the world. Left stick moves along the camera (`mode look`) or the ground (`mode level`), X and B go up and down, left stick press is a 6x boost, the mouse wheel changes the speed. Fall damage cells are kept at his height, so flying down costs nothing. With the endpoint on, the Controls page gets a "Fly mode (test builds)" row (default F7, ini `bind_key_fly`, gamepad `bind_pad_fly`) |
| `cell [<name> [<value>...]]` | List, read or write a named game state cell. A cell is a recipe: a root object, pointer chains found once with `mem path` and a check that tells a right chain from a stale one. Cells: `player.pos`, `player.fall_top`, `player.fall_top2`, `camera.eye`, `camera.target`, `camera.pitch`. A new cell is one entry in `runtime/control_cells.c` |
| `sites` | The known root addresses (`runtime/game_sites.h`) and the value stored at each, as the start for `mem path` |
| `set <key> <value>` | Change a setting in memory. Answers with the new value and `applied: live` or `needs_restart`. Examples: `set upscaler fsr3`, `set antialias taa`, `set vsync off`, `set fps 120`. Enums accept names or indices, booleans on/off, true/false or 1/0. Unavailable backend options are rejected. No settings file is written, but a later menu save may keep the change |
| `frames [seconds]` | Frame intervals of the last seconds (default 5, max 60): count, fps average, frame time average, p50, p95, p99 and max, and the number of frames over 33 and 50 ms. Includes pacing and hitches, not GPU time alone |
| `sample start <seconds> [interval_us]` \| `sample status` | Windows only: background stack capture, written to `build/samples.txt` (analyse with `tools/profile_samples.py`). Refuses overlapping captures |
| `vkprof`, `gpuprof` | Only explain how to turn the profilers on, with `SAVAGEWIND_VK_PROF` and `SAVAGEWIND_GPUPROF_AT` at startup, because they cannot be switched safely at runtime |
| `mem regions` | Readable mappings inside the guest window |
| `mem read <addr> <len> [u8\|u16\|u32\|u64\|s32\|f32\|f64\|hex]` | Read 1 to 4096 bytes |
| `mem write <addr> <type> <value>` | Write one numeric value; rejects inaccessible ranges and values outside the type |
| `mem scan <type> <value> [tolerance] [region=..] [from=.. to=..] [resident=1]`, `mem scan status`, `mem rescan <changed\|unchanged\|increased\|decreased\|equal <v>\|near <v> <tol>>`, `mem list [n]`, `mem reset` | Search guest memory for a value on a background thread and narrow the candidates, like a memory scanner. One session is shared by all clients. Poll `mem scan status` |
| `mem path <root> <target> [depth=1..6] [window=16..65528]` | Find how to reach `target` from the object at `root`, as pointer offsets. Poll `mem path status`. Heap addresses move between runs but layouts do not, so a chain found once keeps working |
| `press <button> [frames]`, `hold <button>`, `release [<button>\|all]`, `stick <l\|r> <x> <y> [frames]`, `input` | Scripted controller input, merged with the real one. Buttons: a b x y l r zl zr plus minus up down left right lstick rstick. Sticks run from -1 to 1, positive y is up |

To use `goto` with a line from an issue, run it in developer mode: `swctl goto "swr:1 x=... y=... z=... yaw=... pitch=... hour=..."`.

`savagewind.log` gets the same line every 30 seconds while Link moves (`[place] swr:1 ...`; none while he stands still), so the log attached to an issue says where its bug happened. `SAVAGEWIND_PLACE_LOG=0` turns these lines off.

## Using swctl

`tools/swctl.py` needs only Python 3.8 or later, with no extra packages.

```
swctl help                 every command with its arguments and a one line description
swctl state                frame number, fps, hour of day, ...
swctl pos                  where Link is
swctl time set 18          set 18:00 (developer)
swctl shot out.png         save a screenshot (developer)
swctl --json state         raw reply, for scripts
swctl --port 7777 ...      other port (default: SAVAGEWIND_CONTROL, else 7777)
swctl --host 127.0.0.1 ... other host (default: 127.0.0.1)
swctl --timeout 5 ...      connect and reply timeout in seconds (default: 5)
swctl --wait 10 state      retry connecting for up to 10 seconds (default: 0)
```

Options go before the command. The default port uses `SAVAGEWIND_CONTROL` only when it is numeric, else 7777.
Ports must be from 1 to 65535. `--wait` retries only connecting, so it never sends a command twice.

Output is plain `key: value` lines, with dotted keys for nested objects and one line per object in a list.
`help` prints an aligned command table. Errors go to stderr. `--json` prints only the original reply line to
stdout, including when the game answers with an error.

Exit code 0 on success, 1 when the game answered with an error, 2 for connection failures, timeouts or invalid
replies. A reply needs its final newline, even if the game closes the connection. With no command, the client
prints usage and suggests `help`.

## Protocol

TCP, text, one request per line, one reply per line, UTF-8, `\n` line ends. Several requests may be sent over one
connection; replies come in the same order.

Request: the command and its arguments separated by spaces. Double quotes keep spaces inside one argument, and
`\"` and `\\` work inside them.

```
get fps
camera
```

Reply: one JSON object.

```
{"ok":true,"data":{"fps":60}}
{"ok":false,"error":"Unknown setting: x"}
```

A command that gives no data answers `{"ok":true,"data":{}}`. Commands run once per presented frame, on the thread
that presents, so they never run in the middle of a frame; a reply can take up to one frame. Unknown commands answer
an error that says so.

Always there: `help` (data: `commands`, a list of `{"name","args","help"}`) and `exit` (ends the game; no data).

## Adding a command

Write a handler in `runtime/control_game.c`, answer with `control_field_*()` for the data and `control_fail()` for an
error, and add it to the table at the bottom with `CONTROL_PLAYER` if it is player safe. `help` and this page pick it up
from there. `control_text()` and `control_copy()` are for front ends that print text or use a clipboard, and are
ignored by the socket.

`make test-control` covers the command layer: who may run what, the queue and the replies.
