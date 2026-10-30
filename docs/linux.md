# Savage Wind on Linux (x86-64, aarch64)

Status (inherited from LASRecomp): builds and runs on Vulkan on x86-64. aarch64 is **experimental**: cross-compiled, its start screen checked under emulation,
the game itself untested on real hardware - community reports decide (see the end). `make appimage` / `make appimages`
package the release AppImages.

## Running the AppImage
    chmod +x Savage_Wind-0.3.0-alpha.3-x86_64.AppImage
    ./Savage_Wind-0.3.0-alpha.3-x86_64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with SSE4.2 (2008 or newer;
without AVX2 / FMA the game code is built in a slower compatible mode).
The start screen asks for the folder with your own game files and sets them up; from then on the game starts
straight away. `--config` (`./Savage_Wind-*.AppImage --config`) opens the configuration screen: Play, Settings, the
game files, Quit. Once the AppImage is in your applications menu (AppImageLauncher, Gear Lever, or your desktop's own
AppImage integration), its entry has a **Savage Wind Configuration** action (right-click it) that does the same. A picture of your own as `splash.jpg` / `splash.png` in the data folder below replaces the start
screen's art.

Everything the app writes lives in `~/.local/share/savagewind` (`$XDG_DATA_HOME/savagewind`): game files (`extracted/`),
saves, settings (`savagewind.ini`), shader caches, imported fonts. Started without a terminal the log is
`~/.local/share/savagewind/savagewind.log` (from a terminal it prints there instead). `SAVAGEWIND_DATA_ROOT=<dir>` uses
another folder. See [Moving the data folder](data-folder.md) for copying existing data and setting up a launch
wrapper, including on Steam Deck.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs. For SDL's own drivers (rumble, LEDs and gyro on PlayStation /
Switch pads) the user needs access to `/dev/hidraw*`, which most distributions give with Steam's udev rules
(`steam-devices`, or Steam installed); without them the controller still works through the kernel driver. Started from
Steam with Steam Input on, the game sees Steam's virtual Xbox controller.

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package (the AppImage runtime
needs `fusermount3`), or run it as `./Savage_Wind-....AppImage --appimage-extract-and-run`.

### Steam Deck motion in Gaming Mode (DSU)

Keep Steam Input enabled for buttons and sticks. On Linux the game automatically prefers fresh native SDL gyro
**and** accelerometer samples, then falls back to a Cemuhook DSU server at `127.0.0.1:26760`. If neither source
works, the existing still-controller / mouse-tilt fallback remains. This is native game motion (bow aiming and
motion puzzles), not gyro-to-mouse; the game's own motion-controls option still decides when aiming uses it.
Controls-page motion sensitivity and inversion apply to either sensor source. No root access is needed by the game.

1. Install [SteamDeckGyroDSU](https://github.com/kmicki/SteamDeckGyroDSU) separately in Desktop Mode. Follow its
   current **Install/Update** instructions: download and run the official **Update GyroDSU** desktop shortcut.
   The game does not install or start the service. Follow the project's installer instructions if it requests
   device permissions. Updating an existing installation uses the same shortcut.
2. Check in Konsole: `systemctl --user status sdgyrodsu.service`. It should be active. The service runs in Gaming
   Mode too. `journalctl --user -u sdgyrodsu.service -n 30 --no-pager` shows its startup and device errors.
3. Add the patched Linux AppImage to Steam as a non-Steam game (native Linux execution; do not force Proton).
   Leave Steam Input enabled and use a normal gamepad layout. No Steam gyro-to-mouse mapping is required.
4. Return to Gaming Mode and launch. DSU fallback is automatic; no launch option is required. To make it explicit
   while keeping a 40 FPS cap, use Steam launch options:

       SAVAGEWIND_DSU=on SAVAGEWIND_FPS=40 %command%

5. Enable the game's motion-controls option, hold the Deck still briefly for bias calibration, then check bow
   aiming and a tilt shrine. Motion should only affect the contexts where the game normally uses it.

`SAVAGEWIND_DSU=auto` is the Linux default: contact DSU only when fresh SDL motion is unavailable.
`SAVAGEWIND_DSU=off` disables DSU completely. `SAVAGEWIND_DSU=on` keeps discovery/subscription active even when SDL
works, with SDL still taking priority. An optional `SAVAGEWIND_DSU_ENDPOINT=127.0.0.1:26760` accepts a numeric IPv4
address and port (no DNS lookup on the game thread). Windows and macOS retain their existing SDL motion path.

Look in `~/.local/share/savagewind/savagewind.log` (or terminal output; custom data roots use their own log) for
`[motion]` and `[dsu]`. SDL availability is logged when opening a controller; `[dsu] connected: slot ...` means
valid motion packets were received; `[motion] source: none -> DSU` means they are being used. `enabled` alone
means the client is trying, not that the server was detected. `SAVAGEWIND_MOTION_LOG=1` adds the existing periodic
fusion/orientation diagnostic. Logs are never emitted per sample by default.

Troubleshooting:

- Buttons working with no gyro is expected when Steam exposes a virtual Xbox pad: check for a DSU connection.
- If DSU stays unavailable, check the service above and its port. If you changed `SDGYRO_SERVER_PORT` on the
  server, set the matching `SAVAGEWIND_DSU_ENDPOINT` on the game. Check that localhost UDP is permitted.
- A timeout means no advancing, valid motion for 500 ms. The client drops stale data and retries discovery and
  subscriptions every second; starting/restarting the service later needs no game restart. A still controller
  remains valid if packets/timestamps keep advancing. Duplicate and out-of-order packets do not refresh it.
- Invalid-packet messages are rate limited: check the endpoint really serves Cemuhook DSU. The known 2.1
  SteamDeckGyroDSU padded version reply is supported explicitly; motion packets still require valid CRC/lengths.
- One DSU controller is selected and retained by slot/MAC until disconnected. With multiple controllers,
  automatic selection cannot infer which physical controller Steam's virtual pad represents; stop unrelated
  DSU sources or use `off` for an external controller without motion. Fresh SDL sensors always take priority.
- After suspend or a source switch, allow a brief stationary period for calibration. Device changes reset fusion
  and timestamps; no stale angular velocity is integrated. Saves and settings do not need to be changed.

Developer validation (no game assets): `DEPS=/absolute/path/to/deps tools/test_dsu.sh`, or omit `DEPS` with SDL3
installed through pkg-config. `SANITIZE=1` enables ASan/UBSan when the compiler's sanitizer runtimes are installed.
Tests exercise CRC against zlib, malformed packets, axis/unit conversion, UDP discovery/subscription, sequence wrap,
server restarts, timeouts, source priority and game-facing source-switch angular velocity. A live diagnostic can be
built separately: `cc -Iruntime tools/dsu_probe.c runtime/dsu.c -lm -o /tmp/dsu_probe`; `/tmp/dsu_probe` checks the
real endpoint for three seconds without SDL or game data. It reports acceleration in the runtime's Switch axes
(flat, face upward: approximately `0 0 -1`) and angular velocity in rad/s.

Protocol/axis references: [Cemuhook reference](https://v1993.github.io/cemuhook-protocol/),
[SteamDeckGyroDSU MIT implementation](https://github.com/kmicki/SteamDeckGyroDSU/blob/master/src/sdgyrodsu/cemuhookadapter.cpp),
and [SDL Steam Deck sensor mapping](https://github.com/libsdl-org/SDL/blob/main/src/joystick/hidapi/SDL_hidapi_steamdeck.c).
The client uses G and rad/s directly in the existing fusion axes; frame timing and `SAVAGEWIND_FPS` are unchanged.

### Steam Deck (first test, 2026-10-23)
SteamOS 3.8, Mesa 26.2 RADV (VANGOGH). The setup works: copying the game files, generating and compiling the game
code (~16 minutes on the Deck's 8 threads), precompiling the shaders (~7 minutes). Known problems:

- (Fixed) The GPU hung within the first frames: black screen, and a process that only a restart of the Deck cleared.
  The game draws point lists with vertex shaders that write no point size, which is undefined without
  `VK_KHR_maintenance5`. The renderer enables it now, so that size is 1.0.
- (Fixed) 15 fps in the open world. Two causes:
  - RADV has no `VK_KHR_unified_image_layouts`, so the renderer copied each target a draw samples while drawing
    into it, ~200 times a frame. Those targets now spend their passes in the `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL`
    layout.
  - amdgpu revisits every page of an imported host-memory buffer at each submission: ~4 ms a submit for the 3 GB
    guest heap. The heap is now GPU memory, exported as a dma-buf and mapped at the heap's addresses
    (`SAVAGEWIND_VK_HEAP_IMPORT=1`: the import, as before).

  Measured standing on the Great Plateau after Continue: about 40 fps with the 60 fps cap, a steady 30 with the 30
  fps cap.
- Shadow resolutions above Console show graphical glitches on RADV.
- Started in Desktop Mode, Steam's desktop controller layout also sends keys and mouse motion. Gaming Mode is untested.

## Building from source
Ubuntu 25.04+ / Debian 13 packages (other distributions: the same libraries under their names; clang 18 or newer - the
lifted FP/SIMD code needs `__builtin_elementwise_*` - and SDL3 3.2 or newer):

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev zlib1g-dev glslc vulkan-tools

Ubuntu 24.04 LTS and older, Debian 12 and openSUSE Leap have no SDL3 package (SDL3 arrived after their freeze):
build SDL3 from source there, or use the AppImage. Vulkan headers older than 1.4.321 (Debian 13, Ubuntu 25.04, Fedora
42 ship 1.4.304 - 1.4.313) build too: feedback loops then use the `ATTACHMENT_FEEDBACK_LOOP_OPTIMAL` layout, as on
drivers without `VK_KHR_unified_image_layouts`.

`tools/linux/distro_build.sh` builds the app (start screen only, no game code) natively in each of Debian 13, Ubuntu
26.04, Fedora 44, Arch and openSUSE Tumbleweed, in containers, with each distribution's own clang, SDL3, FFmpeg,
shaderc and lld; CI runs the same script (`.github/workflows/linux-distros.yml`). That is the check for "does it build
on my distribution": a container reproduces exactly what differs (package names, library sonames, header versions),
where a virtual machine would only do it slower.

(The Vulkan Memory Allocator header the renderer uses is bundled in `third_party/vma`, as are stb_image and
stb_image_write in `third_party/stb`.)

Then from the repo root:

    python3 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn
    make -j         # the start screen only (no game code yet): set up the game files there, or put extracted/ here
    make gen        # once: recompile the game from your own game files
    make -j         # -> build/savagewind and the game code library build/swgame.so

Run `./build/savagewind` from the repo root: with `extracted/` in the starting folder it uses that folder for
everything (dev mode) instead of `~/.local/share/savagewind`.

`ARCH` picks the target, separately from the OS: the generated code differs per arch (aarch64 runs the guest's
FP/SIMD as inline asm, like Apple Silicon; x86-64 lowers it to C with `-march=x86-64-v3`, like Windows, or for CPUs
without AVX2 / FMA `GENARCH_COMPAT`'s `-march=x86-64-v2`: the start screen builds the one the CPU runs, `make` the
`GENARCH` one). `make gen` passes the matching `--simd`. A cross build (`ARCH=aarch64` on an x86-64 machine, `SYSROOT=<arm64 root filesystem>`)
keeps its own `generated-aarch64/` and `build/aarch64/`. The build detects whether shaderc is named
`libshaderc.so` or `libshaderc_shared.so`; override it with `SHADERC=-l<name>` if neither is found.

## AppImages
Needs podman, and for aarch64 QEMU user emulation with binfmt (`sudo apt install podman qemu-user qemu-user-binfmt`).

    make appimage                # this machine's arch -> build/dist/Savage_Wind-<version>-<arch>.AppImage
    make appimage ARCH=aarch64   # cross-compiled from x86-64
    make appimages               # both

Everything is built in Ubuntu 22.04 containers (`tools/linux/Containerfile`), so the result runs on glibc 2.35+:
SDL3, an LGPL-only FFmpeg (only the movie codecs, as on Windows / macOS), and shaderc from pinned sources
(`tools/linux/build_deps.sh`), then the game against them (`tools/linux/package.sh`). aarch64 is cross-compiled in
the amd64 container against the arm64 image's root filesystem (`build/linux/aarch64/sysroot`). The game code
generator helper (`tools/linux/gen_helper.sh`, PyInstaller) is frozen in a container of its own arch (on arm64 under
QEMU).

The AppImage holds `savagewind`, `swgen/`, `zig/`, the start screen font and the bundled libraries (SDL3,
FFmpeg, shaderc). Not bundled: glibc, libstdc++, zlib, the Vulkan loader and the GPU drivers (always the system's). No game
files or fonts.

## Differences from macOS / Windows
- Platform layer `runtime/platform/linux.c`: like macOS, the guest window (0x70_0000_0000..0x73_2000_0000) is one
  read/write mapping (`MAP_NORESERVE`), crashes are caught with `sigaction` on an alternate stack.
- The HLE / service / reset tables are ELF sections found through the linker's `__start_` / `__stop_` symbols.
- Renderer: the Windows Vulkan backend unchanged.
- Without the user's Switch fonts the game text uses DejaVu Sans (TrueType; the game's font engine can't use CFF
  fonts such as Noto Sans CJK).
- Pages: 4K and 16K kernels (Asahi) work; 64K-page kernels (some ARM servers) are not supported.

## Testing on ARM (aarch64): what to report
The aarch64 AppImage is experimental: built on x86-64, only its start screen has run (emulated): reports from real hardware
(Asahi Linux, Snapdragon laptops, Raspberry Pi 5 and other boards) are what tells whether it works. Please include:

1. **Distribution and kernel**: `cat /etc/os-release | head -3; uname -r; getconf PAGESIZE`
2. **GPU and Vulkan driver**: `vulkaninfo --summary` (package `vulkan-tools`); at least the devices section with
   `deviceName`, `driverName` and `driverInfo`.
3. **What happened**: start screen? extraction? in game? where it stopped, any visual problems (screenshots help).
   With a controller: its model, and whether it works (the log names it: `[input] gamepad: ...`).
4. **The log**: `~/.local/share/savagewind/savagewind.log` (or the terminal output when started from a terminal).
   A crash writes `*** signal ...` and a backtrace there; attach the whole file. Opened again since? The run before is
   `savagewind.prev.log` next to it.
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./Savage_Wind-0.3.0-alpha.3-aarch64.AppImage --appimage-extract    # -> squashfs-root/
       cd ~/.local/share/savagewind
       gdb -batch -ex 'handle SIGSEGV nostop noprint pass' -ex run -ex 'thread apply all bt 20' \
           --args ~/squashfs-root/usr/bin/savagewind > gdb.txt 2>&1

   (Adjust the `squashfs-root` path to where you extracted it. The game itself uses SIGSEGV for memory watches, hence
   the `handle` line; a real crash still ends in the backtrace. Attach `gdb.txt`.)
