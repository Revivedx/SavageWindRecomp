# Command line

`savagewind <command>` does what the start screen and Savage Wind Configuration do, without a window, so scripts
and other launchers can drive Savage Wind. `savagewind help` lists the commands and `savagewind help <command>`
explains one. A first argument that isn't a command keeps its old meaning (the game files folder), and `--play`,
`--config` and the other flags work as before.

| Command | What it does | Exit status |
|---|---|---|
| `setup [<folder>]` (`install`, `build`) | Sets the game files up from a folder with the extracted `exefs/` and `romfs/` (and the DLC's RomFS), as **Set up game files** does: checks, copies, builds the game code, and precompiles the shaders when that option is on. With no folder, builds the game code again from the copy already set up. | 0 ready, 1 failed, 130 stopped |
| `run [--profile <name>]` (`play`) | Starts the game. Fails without opening anything when the game files aren't ready. `--profile` switches the save profile first (it stays active, as on the Saves tab). | the game's |
| `status` | Whether the game files are ready, their version, the main build ID and the DLC. | 0 ready |
| `verify` | Every check in full: the files and their version, every RomFS file and byte, the movies, the game code, and the DLC read through. | 0 all fine |
| `delete --yes` | Deletes the game files and the game code built from them. Never the saves, settings or shaders. | 0 deleted, or nothing to delete |
| `settings [list \| get <key> \| set <key> <value>]` | The settings in `savagewind.ini`. A value Savage Wind doesn't take is refused, and the file stays as it was. Changes apply at the next start. | 0 done |
| `launch-options [list \| available [--all] \| set <NAME> [<value>] \| remove <NAME> \| clear]` (`flags`) | The launch options (Settings > Launch options). The ones Savage Wind reserves for itself are refused with the reason. | 0 done |
| `saves [list \| backups \| backup \| use <p> \| create <p> \| restore <backup> --yes]` | Save profiles and backups. `restore` never deletes: the current saves move into the backups first, as a new dated backup. It's refused while the game runs. | 0 done |
| `paths` | Where the data root, game files, settings, launch options, saves, backups, log and shader cache are. | 0 |
| `shaders` | Whether the shaders are precompiled, how many, when, and whether for this version and driver. | 0 |
| `precompile-shaders` | Precompiles every shader. | 0 done |
| `update` | Asks SW Update whether there's a newer release (releases only). It doesn't install it: Configuration does. | 0 checked |
| `log [--follow \| --previous]` | Prints `savagewind.log` (or the run before's), or follows it. | 0 |
| `config` | Opens Savage Wind Configuration. | |
| `add-to-steam` | Adds Savage Wind to Steam as a non-Steam game. | 0 added |
| `completion bash \| zsh \| fish` | Prints a shell completion script. | 0 |
| `help [<command>]`, `version` | | 0 |

Usage mistakes (a missing `--yes`, an unknown subcommand) exit with 2.

## Output

A command writes its own output to stdout, in plain lines (setup's progress messages are in the menu language). The
log keeps the detail. A command adds to `savagewind.log` instead of starting a new one, so the game's last log
stays. In a terminal the log isn't echoed while a command runs: `--verbose` shows it. `run` and `config` start as
usual.

Only `run`, `config`, `setup` and `precompile-shaders` start with the saved launch options. The others only read
or change files.

## For launchers: `--json`

`setup`, `status`, `verify`, `settings`, `launch-options`, `saves`, `paths`, `shaders` and `update` take `--json`.

`setup --json` prints one JSON object per line:

```
{"event":"progress","stage":"copy","percent":3,"done":452984832,"total":15129626830,"step":"Copying the game files...","text":"..."}
{"event":"progress","stage":"build","percent":40,"step":"...","text":"..."}
{"event":"progress","stage":"shaders","percent":95,"step":"Precompiling shaders...","text":"..."}
{"event":"done","ok":true,"message":"Game files ready.","hint":"","text":"..."}
```

- `stage` is `copy`, `build` (the game code) or `shaders`. `done` and `total` (bytes) come with `copy`.
- `step` and `message` are in English, for matching. `text` is the same in the menu language, for showing.
- On failure `ok` is `false` and `hint` says what to do.
- Ctrl+C or SIGTERM stops it: `{"event":"done","ok":false,"stopped":true,...}` and exit status 130. The compilers
  are ended with it. The unfinished copy shows in `status`, and the next `setup` replaces it.

`status --json`:

```
{"ready":true,"problem":"","game_files":"extracted","version":"1.9.0","main_build_id":"cd57b23fa4bbad65","dlc":"ready","savage_wind":"0.5.3-alpha.5"}
```

`verify --json`:

```
{"ok":true,"version":"1.9.0","files":"","romfs":"","movies":true,"game_code":true,"dlc":"ready","dlc_problem":""}
```

`SAVAGEWIND_DATA_ROOT` chooses the data root, as it always has.

## Testing

`make test-cli` (`tools/test_cli.py`) runs every command against a scratch data root in `build/test_cli/`. No game
files are needed, and the player's own data root is never touched. It checks the exit statuses, that every `--json`
output parses, and the commands that change files: settings taken and refused, launch options, save profiles, and a
synthetic backup restored without losing the saves it replaces.
