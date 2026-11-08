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
| `rupees [<0-9999> \| lock \| unlock]` | Cheat. Without an argument: whether Infinite rupees is on and the last wallet count read. `<n>` sets the wallet once; `lock` turns on Infinite rupees (the wallet kept at 9999, the same as the overlay's Cheats tab) and `unlock` turns it off. The rupee count is found in the game's memory by a background thread, so the first write takes a few seconds (`runtime/rupees.c`). Like `set`, no settings file is written |
| `shot <file>` | Save a screenshot as PNG, on the machine the game runs on (Vulkan only for now). The file must be a `.png` inside the game's folder: a relative path without `..` |
| `goto <x> <y> <z>` \| `goto <swr:1 line>` \| `goto <place>` | Go to a place from `pos` or `report`: move Link, set the hour of the line (the clock keeps running, or stays frozen if it was) and turn the camera to its yaw and pitch like `look`. Same limits as `teleport`. Or go to a named place: `goto shrine 5`, `goto korok H23`, `goto stable wetland`, `goto tower 7`, `goto hateno`, `goto nearest korok` (see "Named places" below). Link arrives 2 to 3 units above the ground and drops the rest |
| `tp ...` | Same as `goto`, with the same arguments |
| `places [<kind>] [words]` | List the places `goto` knows: without arguments the kinds and how many there are, with a kind (`shrine`, `tower`, `stable`, `village`, `landmark`, `korok`) their numbers and names, and words narrow the list (`places shrine akkala`). Shows 40 at most |
| `teleport <x> <y> <z>` \| `teleport status` \| `teleport forget` | Move Link. Writes the real position, reached through a pointer chain from `PlayerInfo` (found with `mem path`, checked against `pos`). If the chain does not match it falls back to a background search that never blocks a frame; poll `teleport status` (`scanning`, `probing`, `testing`, `verifying`, `settling`, `done`, `failed`). A jump of 300 units or more also holds Link at the target (`settling`) until the game has loaded the map around him: the game pauses the world while it does and loads it with the world's own update, so without the hold a far jump froze the world for good (the hold needs the `tp_gate` patch of the generated game code). Stand still when you start; do not use it while riding, in a cutscene or loading. A target below the ground makes Link fall. `forget` drops the saved address |
| `look <yaw> <pitch>` \| `look status` \| `look stop` | Turn the camera (degrees, either may be `keep`, pitch within -89..89). Writes the camera cells and checks again each frame, because the game moves the eye a little after a write. Without the cells it falls back to the right stick (needs the TAA camera read). Returns at once, poll `look status` (`running`, `done`, `timeout`, `failed`) |
| `fly [on \| off \| toggle \| mode look \| level \| speed <n>]` | Fly mode: Link floats and the controls steer him through the world. Left stick moves along the camera (`mode look`) or the ground (`mode level`), X and B go up and down, left stick press is a 6x boost, the mouse wheel changes the speed. Fall damage cells are kept at his height, so flying down costs nothing. With the endpoint on, the Controls page gets a "Fly mode (test builds)" row (default F7, ini `bind_key_fly`, gamepad `bind_pad_fly`) |
| `cell [<name> [<value>...]]` | List, read or write a named game state cell. A cell is a recipe: a root object, pointer chains found once with `mem path` and a check that tells a right chain from a stale one. Cells: `player.pos`, `player.fall_top`, `player.fall_top2`, `camera.eye`, `camera.target`, `camera.pitch`. A new cell is one entry in `runtime/control_cells.c` |
| `sites` | The known root addresses (`runtime/game_sites.h`) and the value stored at each, as the start for `mem path` |
| `set <key> <value>` | Change a setting in memory. Answers with the new value and `applied: live` or `needs_restart`. Examples: `set upscaler fsr3`, `set antialias taa`, `set vsync off`, `set fps 120`. Enums accept names or indices, booleans on/off, true/false or 1/0. Unavailable backend options are rejected. No settings file is written, but a later menu save may keep the change |
| `frames [seconds]` | Frame intervals of the last seconds (default 5, max 60): count, fps average, frame time average, p50, p95, p99 and max, and the number of frames over 33 and 50 ms. Includes pacing and hitches, not GPU time alone |
| `prof [start \| stop \| toggle \| status \| report]` | The on-demand profiler: records zones, counters, GPU time and stack samples into a capture file, reported with `tools/swprof.py` (see `docs/profiling.md`). `status` shows the state and the last file |
| `sample start <seconds> [interval_us]` \| `sample status` | Windows only: background stack capture, written to `build/samples.txt` (analyse with `tools/profile_samples.py`). Refuses overlapping captures |
| `vkprof` | Only explains how to turn the Vulkan timers on, with `SAVAGEWIND_VK_PROF` at startup, because they cannot be switched safely at runtime |
| `gpuprof` | GPU time per pass, dispatch and copy of the next frame, as `[gpuprof]` lines in the log (the capture `SAVAGEWIND_GPUPROF_AT` takes at a fixed frame). The capture waits for the GPU once, a short hitch. The `prof` command's capture has the same rows in the `gpu` thread |
| `threads` | CPU time used so far (ms) by each guest thread (`guest.<handle>_<entry>`) and by the render threads (`host.render`, the NVN queue thread; `host.recorder` when the recorder runs), with `now_ms` and `presents`. Two calls give each thread's CPU time per frame. Some threads spin while they wait, so CPU time is not work: `tools/perf_routes.py` takes the work share from `sample` |
| `mem regions` | Readable mappings inside the guest window |
| `mem read <addr> <len> [u8\|u16\|u32\|u64\|s32\|f32\|f64\|hex]` | Read 1 to 4096 bytes |
| `mem write <addr> <type> <value>` | Write one numeric value; rejects inaccessible ranges and values outside the type |
| `mem scan <type> <value> [tolerance] [region=..] [from=.. to=..] [resident=1]`, `mem scan status`, `mem rescan <changed\|unchanged\|increased\|decreased\|equal <v>\|near <v> <tol>>`, `mem list [n]`, `mem reset` | Search guest memory for a value on a background thread and narrow the candidates, like a memory scanner. One session is shared by all clients. Poll `mem scan status` |
| `mem path <root> <target> [depth=1..6] [window=16..65528]` | Find how to reach `target` from the object at `root`, as pointer offsets. Poll `mem path status`. Heap addresses move between runs but layouts do not, so a chain found once keeps working |
| `press <button> [frames]`, `hold <button>`, `release [<button>\|all]`, `stick <l\|r> <x> <y> [frames]`, `input` | Scripted controller input, merged with the real one. Buttons: a b x y l r zl zr plus minus up down left right lstick rstick. Sticks run from -1 to 1, positive y is up |

To use `goto` with a line from an issue, run it in developer mode: `swctl goto "swr:1 x=... y=... z=... yaw=... pitch=... hour=..."`.

### Named places

`goto` also takes a kind and a number or a name. It finds a shrine, tower, stable, village, landmark or Korok without looking up any coordinates:

```
swctl goto shrine 5            the shrine with the game's number 5 (Ne'ez Yohma)
swctl goto shrine oman au      by name; apostrophes and capital letters do not matter
swctl goto shrine pushing power   or by the title the game gives the shrine's trial
swctl goto korok H23           a Korok by the game's own id (see below)
swctl goto korok 10            the 10th of 900, in the order `places korok` shows
swctl goto stable 2            the 2nd stable in alphabetical order (East Akkala)
swctl goto tower 7             the game's tower number (7 is Great Plateau Tower)
swctl goto stable wetland      or: swctl goto wetland
swctl goto hateno              names without a kind work when only one place has that name
swctl goto nearest korok       the closest of a kind to where Link is now (works for every kind)
```

If a name matches several places, the reply names them. `places <kind>` prints the numbers and names. What the numbers mean:

- Shrines use the game's own number (`Dungeon005` is shrine 5, 0 to 135; 120 and above are the DLC shrines). That is the number in the game's data and on the ZeldaMods object map, not an order the game shows to players.
- Towers use the game's tower number (1 to 15).
- Stables, villages and landmarks are numbered alphabetically by their English name.
- Koroks have no number in the game. Each one has an id like `H23` (the id the object map uses). `korok 10` counts the 900 in map order: square `A-1`, `A-2` ... then by id. `places korok` shows the order.

The coordinates are the game's world coordinates, the same frame as `pos`. The table is `runtime/control_places_data.inc`, generated by `python3 tools/gen_places.py` (needs network access). Two sources feed it:

- 154 places (120 shrines, all 15 towers, the villages, some stables and landmarks, Hyrule Castle) use the game's own start positions: the `StartPos` entries of `Map/MainField/Static.smubin` in `Bootup.pack`, which is where the game puts Link when he warps there, so it is ground he can stand on. `tools/dump_startpos.py` reads them from your own copy of the game (`python3 tools/dump_startpos.py --pack <Bootup.pack>`) and writes `tools/places_startpos.json`, which holds only coordinates and is checked in.
- 15 more places (14 DLC shrines, the Forgotten Temple) use `tools/places_measured.json`: the height Link really stood at after a `goto` dropped him from the marker position, measured in a running game with `tools/check_places.py` (marker heights were up to 100 units too high there).
- Everything else (the 2 other DLC shrines, the remaining stables and landmarks, all Koroks) uses the data behind the [ZeldaMods object map](https://objmap.zeldamods.org) (extracted from the game files of Breath of the Wild; the map is open source at [zeldamods/objmap](https://github.com/zeldamods/objmap)): the position of the place's marker or of the Korok. Those are not standing spots.

`make test-places` tests the lookup.

`goto` adds a small lift above the table height (1 unit for the places above that stand on ground, 2 for Koroks, 3 for the rest), so Link arrives in the air and drops onto the ground. For 4 seconds after any arrival the fall damage cells follow his height, so a place that is a little too high costs no hearts.

After a far jump the screen can look wrong for a second or two (dark or stale picture, camera low in the grass or inside the terrain): the game pauses the world while it loads the map around the new spot and only then settles the camera. It corrects itself. The camera values (`camera.eye` and the rest) cannot be read yet at that point, so `goto` cannot fix it earlier.

Known crash: the game itself can crash on a loader thread while it builds a model (`access violation: write at 0x8`). It is not caused by `goto`: it also happens on a plain load of a save in the Gerudo desert, about one load in five. The model loader makes a temporary heap from the model's ResourceSizeTable entry and does not check when an allocation in it fails; a few big models (`FldObj_RuinStatueKnight_A-03`) overflow it. Giving every model's entry 64 KB more stopped it in 40 loads in a row. The fix is in PR #524 (`SAVAGEWIND_RSTB_MODEL_KB`); without it, start the game again.

Limits: places that still use marker data can put Link inside a wall, a rock or under the ground, or high above it. Use `pos` afterwards to check where he is. Move him with `teleport` if he is stuck.

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
