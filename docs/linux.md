# Savage Wind on Linux (x86-64, aarch64)

Status (inherited from LASRecomp): builds and runs on Vulkan on x86-64. aarch64 is **experimental**: cross-compiled, its start screen checked under emulation,
the game itself untested on real hardware - community reports decide (see the end). `make appimage` / `make appimages`
package the release AppImages.

## Running the AppImage
    chmod +x Savage_Wind-beta_3-x86_64.AppImage
    ./Savage_Wind-beta_3-x86_64.AppImage

Needs: a distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36, Arch, ...), a Vulkan 1.3 driver
(Mesa RADV / ANV / NVK / Turnip / Asahi Honeykrisp, or NVIDIA's), and on x86-64 a CPU with AVX2 (2013 or newer).
The start screen asks for the folder with your own game files and sets them up; from then on the game starts
straight away. `--config` (`./Savage_Wind-*.AppImage --config`) opens the configuration screen: Play, Settings, the
game files, Quit. Once the AppImage is in your applications menu (AppImageLauncher, Gear Lever, or your desktop's own
AppImage integration), its entry has a **Savage Wind Configuration** action (right-click it) that does the same. A picture of your own as `splash.jpg` / `splash.png` in the data folder below replaces the start
screen's art.

Everything the app writes lives in `~/.local/share/savagewind` (`$XDG_DATA_HOME/savagewind`): game files (`extracted/`),
saves, settings (`savagewind.ini`), shader caches, imported fonts. Started without a terminal the log is
`~/.local/share/savagewind/savagewind.log` (from a terminal it prints there instead). `SAVAGEWIND_DATA_ROOT=<dir>` uses
another folder.

Controllers work as on the other systems (SDL3's gamepad support, bundled): Xbox, PlayStation (DualShock 4,
DualSense), Switch Pro / Joy-Con, 8BitDo and other pads through the kernel's drivers, plugged in or paired while the
game runs. For SDL's own drivers (rumble, LEDs and gyro on PlayStation /
Switch pads) the user needs access to `/dev/hidraw*`, which most distributions give with Steam's udev rules
(`steam-devices`, or Steam installed); without them the controller still works through the kernel driver. Started from
Steam with Steam Input on, the game sees Steam's virtual Xbox controller.

If the AppImage won't start with a FUSE error, install your distribution's `fuse3` package (the AppImage runtime
needs `fusermount3`), or run it as `./Savage_Wind-....AppImage --appimage-extract-and-run`.

## Building from source
Ubuntu 25.04+ / Debian 13 packages (other distributions: the same libraries under their names; clang 18 or newer - the
lifted FP/SIMD code needs `__builtin_elementwise_*` - and SDL3 3.2 or newer):

    sudo apt install clang lld make pkg-config python3-venv libsdl3-dev libavformat-dev libavcodec-dev \
        libavutil-dev libswscale-dev libvulkan-dev libshaderc-dev glslc vulkan-tools

Ubuntu 24.04 LTS and older, Debian 12 and openSUSE Leap have no SDL3 package (SDL3 arrived after their freeze):
build SDL3 from source there, or use the AppImage. Vulkan headers older than 1.4.321 (Debian 13, Ubuntu 25.04, Fedora
42 ship 1.4.304 - 1.4.313) build too: attachment feedback loops stay off there and the renderer copies the targets
instead (the same path as `SAVAGEWIND_VK_NO_FEEDBACK_LOOP=1`).

`tools/linux/distro_build.sh` builds the app (start screen only, no game code) natively in each of Debian 13, Ubuntu
25.04, Fedora 42, Arch and openSUSE Tumbleweed, in containers, with each distribution's own clang, SDL3, FFmpeg,
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
FP/SIMD as inline asm, like Apple Silicon; x86-64 lowers it to C with `-march=x86-64-v3`, like Windows). `make gen`
passes the matching `--simd`. A cross build (`ARCH=aarch64` on an x86-64 machine, `SYSROOT=<arm64 root filesystem>`)
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
FFmpeg, shaderc). Not bundled: glibc, libstdc++, the Vulkan loader and the GPU drivers (always the system's). No game
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
   A crash writes `*** signal ...` and a backtrace there; attach the whole file.
5. **A crash backtrace with gdb** when the log's isn't enough:

       ./Savage_Wind-beta_3-aarch64.AppImage --appimage-extract    # -> squashfs-root/
       cd ~/.local/share/savagewind
       gdb -batch -ex 'handle SIGSEGV nostop noprint pass' -ex run -ex 'thread apply all bt 20' \
           --args ~/squashfs-root/usr/bin/savagewind > gdb.txt 2>&1

   (Adjust the `squashfs-root` path to where you extracted it. The game itself uses SIGSEGV for memory watches, hence
   the `handle` line; a real crash still ends in the backtrace. Attach `gdb.txt`.)
