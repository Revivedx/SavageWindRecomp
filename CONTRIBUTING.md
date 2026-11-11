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
- no code, headers or documentation copied from decompilation projects (such as ZeldaRET): Savage Wind's code is
  written from what we find in our own copy of the game, and the game's code itself is only ever recompiled on the
  player's machine

Pull requests that include Nintendo files can't be merged and will be closed. Screenshots and short clips of the
game in bug reports are fine, and so are your own save files, zipped and attached to the issue: a crash or a problem
with the game's mechanics can often only be followed from the save. Saves are for issues: don't commit them.

## Moderation

Savage Wind has no public Discord or other community channels: bugs, requests and code all go through GitHub. If you
want to contribute and coordinate with the rest of the contributors, you can contact the maintainer on Discord
(`@ReviveDx`) or through [revi.contact](https://revi.contact). It's one person's project with the help of
contributors, and every contribution is reviewed and tested by the maintainer before it goes into `test`.

Issues, pull requests and comments that share, link to or ask for copies of the game, keys, firmware or dumping and
decryption tools are removed. An account that does it again is blocked.

Issues are for bug reports and feature requests. Don't open an issue to share code, patches or changed files: open a
pull request against `test` instead (see [Pull requests](#pull-requests)), so the change can be reviewed, tested and
credited to you. Issues that share code are closed with a pointer here, and an account that keeps opening them after
that is blocked.

## Which branch to work on

- **`test`** is where development happens. It has every change that is implemented and being tested, ahead of the
  latest release: the most up-to-date code, but not an official release.
- **`main`** only moves for a release. Once `test` has been tested well, it goes into `main` through a pull request
  and the release is published from it.
- **Base your work on `test` and open pull requests against `test`.** `main` is an older state, so work based on it
  often repeats what `test` already has. Before you start, look at `test`: its [Status](docs/STATUS.md) page says what
  is done, in progress and planned.

## Read the FAQ before opening an issue

The [FAQ](docs/FAQ.md) answers the common questions and lists the known issues. An issue about something it already
covers is closed right away, with a comment that says why and links the entry:

- **A question the FAQ answers**, or a problem it gives a solution or workaround for.
- **A new report of a known issue:** closed as a duplicate and pointed to the issue that tracks it. Add what you know
  there. The tracking issues themselves stay open.
- **Performance or graphics on an older or integrated GPU:** goes on
  [#589](https://github.com/Revivedx/SavageWindRecomp/issues/589).
- **Something that breaks at high frame rates:** goes on
  [#425](https://github.com/Revivedx/SavageWindRecomp/issues/425).
- **A request the FAQ has already answered**, such as other platforms, other versions of the game or multiplayer.

If you tried what the FAQ says and the problem is still there on the latest release, say so in your report and attach
`savagewind.log`, and it stays open. If yours was closed and that is the case, comment with the log and it is reopened.

## Reporting bugs

Open a GitHub issue with:

- your platform (Windows / Linux), hardware (CPU, GPU) and window size
- the Savage Wind version: a release (e.g. 0.2.0-alpha.2), or the `test` commit you built
- the game version (the `main` build id that `python tools/sw_probe.py` prints; 1.9.0 is the only one supported)
- what you did, what happened, and what you expected
- a screenshot or short clip if it's visual
- for crashes: the log the app prints to the terminal (run it from a terminal to capture it)

## Building

See [docs/BUILD-windows.md](docs/BUILD-windows.md) for Windows and
[docs/linux.md](docs/linux.md) for Linux. In short: put your own 1.9.0 files in `extracted/`, `make gen` once to
recompile the game code from them, then `make -j`.

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

- Open it against `test`, on top of its latest commit (see "Which branch to work on").
- Keep each pull request focused on one change; small is easier to review.
- Describe what changed, why, and how you tested it.
- Don't include personal paths or information in code, comments or commit messages.

## License

By contributing, you agree that your contributions are licensed under the project's [MIT License](LICENSE).
