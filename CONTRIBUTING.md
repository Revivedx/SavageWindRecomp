# Contributing to Savage Wind Recomp

Thanks for wanting to help! Bug reports, testing on different hardware and code contributions are all welcome.

> [!NOTE]
> This project is developed mostly with AI (see the README). Contributions written by hand, with AI assistance or
> anything in between are equally welcome - what matters is that a change works, is tested, and follows the rules
> below.

## The one hard rule: no Nintendo files

Nothing owned by Nintendo may ever be committed or attached to an issue or pull request:

- no copies of the game in any form, no keys, no firmware
- no extracted game files (`extracted/`), no generated code (`generated/`), no fonts (`.bfttf` / Switch `.ttf`)
- no game assets of any kind: textures, models, shaders, music, text, level files - also not "just one file for
  testing"
- no links to any of these, and no instructions for dumping or decrypting them

Pull requests that include Nintendo files can't be merged and will be closed. Screenshots and short clips of the
game in bug reports are fine.

## Reporting bugs

Open a GitHub issue with:

- your platform (Windows / Linux), hardware (CPU, GPU) and window size
- the game version (the `main` build id that `python tools/sw_probe.py` prints; 1.8.2 and 1.9.0 are the ones
  supported, `tools/game_symbols.py`)
- what you did, what happened, and what you expected
- a screenshot or short clip if it's visual
- for crashes: the log the app prints to the terminal (run it from a terminal to capture it)

## Building

See the README's "Building from source", [docs/BUILD-windows.md](docs/BUILD-windows.md) for Windows and
[docs/linux.md](docs/linux.md) for Linux. In short: put your own 1.8.2 or 1.9.0 files in `extracted/`, `make gen`
once to recompile the game code from them, then `make -j`.

## Project layout

- `tools/gen.py` - the recompiler: translates the game's executables into C (`generated/`, never committed).
  `tools/layout.py` places the modules; `tools/ghidra/` queries a Ghidra project of the game (yours, built locally).
- `runtime/` - the host side:
  - `kernel.c`, `loader.c`, `ipc.c`, `services/` - the Switch OS replacement
  - `hle/` - host replacements (NVN bootstrap, input, movies, heaps, ...)
  - `nvn/` - the graphics layer: command recording (`cmdbuf.c`), objects (`objects.c`), the Vulkan backend
    (`vulkan/`), the Maxwell shader translator (`maxwell/translate.c`); `metal.m` / `renderer.m` are LAS Recomp's
    macOS backend, kept but not maintained here (Windows and Linux are the targets)
  - `ui/` - the ImGui overlay menu and the start screen
- `docs/SAVAGE-WIND.md` - developer notes (in Spanish): what changed from LAS Recomp, the boot so far, debugging
  switches (`SAVAGEWIND_*` environment variables).

## Testing your change

- Build cleanly (`make -j`). If you touch rendering, say which GPU and driver you tested on.
- Test in game, and look at the result: a fix isn't verified until you've seen it work. `SAVAGEWIND_SHOT_AT=<frame>`
  saves screenshots; many more debug hooks are documented where they're defined (grep for `getenv("SAVAGEWIND_`).
- State that belongs to one boot must be reset when the game reboots in place: use an `RT_ON_RESET` hook (see
  `runtime/hle.h`).

## Code style

- Match the surrounding code: its naming, structure and comment density. Comments explain *why* (what the game does,
  what an address is, which bug showed the problem), not what the next line does.

## Pull requests

- Keep each pull request focused on one change; small is easier to review.
- Describe what changed, why, and how you tested it.
- Don't include personal paths or information in code, comments or commit messages.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT License](LICENSE).
