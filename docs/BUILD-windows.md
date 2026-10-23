# Building Savage Wind on Windows (x86-64)

Status: part of main. Builds and runs the game on Vulkan (with your own game files); `make dist` packages the
release zip.

## One-time setup
Run in PowerShell from the repo root:

    powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1

This installs MSYS2 (UCRT64: clang, lld, make, SDL3, FFmpeg, Vulkan loader + headers, shaderc, Vulkan Memory
Allocator, validation layers), Python 3.12 for the tools (`tools\venv`: unicorn, capstone, lz4) and switches the
clone to LF line endings.

## Build
From an MSYS2 UCRT64 shell, or Git Bash with MSYS2 first on PATH:

    export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
    make gen        # once: recompile the game from extracted/exefs (your own game files; see the start screen)
    make -j         # -> build/savagewind.exe and the game code library build/swgame.dll

Without `generated/` only the app is built: the start screen builds the game code itself when you set up the game
files (as for players), given the generator and the compiler (`make gen-helper`, `tools/packaging/fetch_zig.sh`).
`make runtime-objs` compiles the runtime only (quick check).

Run from a shell that has `C:\msys64\ucrt64\bin` on PATH (SDL3, FFmpeg, Vulkan, shaderc DLLs).

## Package
    make deps-windows   # once: FFmpeg built as LGPL only (build/deps-win; downloads its source, needs nasm)
    make gen-helper     # the game code generator without Python (build/helper/swgen)
    tools/packaging/fetch_zig.sh  # the compiler the start screen builds the game code with (build/zig)
    make dist           # -> build/dist/SavageWind-<version>-windows.zip (no game code inside)

`make gen-helper` freezes the generator with a python.org CPython (`py -3.12` by default; another one with
`GEN_PY="py -3.14"`, as long as unicorn and capstone have wheels for it). `make dist` also makes the slim compiler
(`tools/packaging/zig_slim.sh`: only the headers the game code includes, and zig's runtime built ahead of time), so the
start screen's build doesn't compile zig's own libraries on the player's machine. The version is
`SAVAGEWIND_VERSION` in `runtime/ui/launcher.mm`. Before a release, test the zip as a player would: extract it to a
new folder, run it with a PATH without MSYS2, set up the game files from the start screen and play.

The zip's `Savage Wind/` folder holds `savagewind.exe` (icon from `docs/icon/icon.ico`, via windres),
`Savage Wind Configuration.exe` (`build/savagewind-config.exe`, runtime/platform/windows_config.c: starts
`savagewind.exe --config`, the configuration screen; same icon), the Fira Sans font files, `swgen/`, `zig/` and the
MSYS2 DLLs. A plain start of `savagewind.exe` shows only the game files setup until they're ready, then goes
straight into the game; `--config` shows the configuration screen (Play, Settings, game files, Quit). Never in the
zip: a start screen picture (`splash.jpg` / `splash.png` is the player's own, read from the data folder; `make dist`
checks). No installer: game files, saves and settings are kept next to the exe, so
the folder must be writable (the app says so and quits if it isn't, e.g. under Program Files). A source checkout
with `extracted/` in the starting folder keeps using that folder instead.

## Differences from macOS
- Guest window 0x70_0000_0000..0x73_2000_0000 is reserved with VirtualAlloc and committed per region
  (kernel regions, modules, HLE heap); stray first touches are committed by a vectored exception handler.
- AArch64 FP/SIMD instructions are lowered to C (`gen.py --simd=c`, the default on non-arm64 hosts) instead of
  inline-asm passthrough.
- Renderer: Vulkan (runtime/nvn/vulkan) instead of Metal; shaders are translated to GLSL and compiled with
  shaderc. macOS keeps Metal.
- "Quit to Start Screen" reboots the game in place on both (runtime/main.c); the only re-exec left is
  the startup retry when the guest window is taken, which on Windows starts a new process and exits.

## Tests
- `tools/tests/test_translate.sh`: shader translator fuzz (MSL output unchanged, GLSL compiles and validates).
- `tools/tests/test_vk_pipelines.sh`: fuzzed shaders through the backend's compile/reflection into real pipelines.
- `SAVAGEWIND_VK_VALIDATION=1` with `VK_ADD_LAYER_PATH` pointing at `C:\msys64\ucrt64\bin`: Khronos validation layer.
- Run test windows directly, not under MSYS `timeout` (it stalls the window).
