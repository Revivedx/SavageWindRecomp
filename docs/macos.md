# Savage Wind on macOS (Apple Silicon)

Status: runs on the same Vulkan renderer as Windows and Linux, on Apple's Metal through
[MoltenVK](https://github.com/KhronosGroup/MoltenVK). Checked on a MacBook Air M4 (24 GB,
macOS 27): the start screen, loading a save and play sessions of several minutes in the overworld at a locked 60 fps
at 1080p (SMAA), without a crash. A new game and the intro are not checked yet. The game's
code is AArch64 like the Switch's, so its FP/SIMD runs as the guest's own instructions (`gen.py --simd asm`, checked
against Unicorn by `tools/test_simd.py --simd asm`). Intel Macs are not supported.

(The Metal backend inherited from LAS Recomp is not part of this: unmaintained, it builds only with `RENDERER=metal`.)

## Building from source

Needs: an Apple Silicon Mac, the Xcode command line tools (`xcode-select --install`) and [Homebrew](https://brew.sh).

    brew install sdl3 ffmpeg python@3.12 molten-vk vulkan-loader vulkan-headers shaderc
    git clone -b test https://github.com/Revivedx/SavageWindRecomp.git && cd SavageWindRecomp
    python3.12 -m venv tools/venv && tools/venv/bin/pip install lz4 capstone unicorn

Put your own extracted game files in `extracted/` (`exefs/`, `romfs/`, and the DLC's `aoc1/` / `aoc2/` if you have it:
see [game-files.md](game-files.md)), then:

    make gen                        # the game code, from your files (a few minutes)
    make -j$(sysctl -n hw.ncpu)
    ./build/savagewind extracted

Homebrew's Vulkan loader finds MoltenVK by itself: no `VK_ICD_FILENAMES` / `VK_DRIVER_FILES` is needed.

The first start builds every pipeline as it is first drawn (see "Differences" below). Precompiling them once avoids
that: Game Files > Precompile shaders on the start screen, or

    SAVAGEWIND_HIDDEN=1 ./build/savagewind --precompile-shaders extracted

(about 100 s on an M4 the first time; later starts load the cache in under a second).

## The .app

    make deps                       # SDL3, FFmpeg, Python, the Vulkan loader and shaderc for macOS 15+, from pinned sources
    make gen-helper                 # the game code generator the start screen runs
    tools/fetch_zig.sh              # the compiler it builds the game code with
    make app                        # build/Savage Wind.app (MoltenVK and its manifest inside), ad-hoc signed
    make dmg                        # build/dist/Savage Wind-<version>-macOS.dmg

The bundle holds no game code: as on Windows and Linux the start screen asks for the folder with your game files, sets
them up and builds the game code on your Mac. Everything it writes lives in
`~/Library/Application Support/local.savagewind`: game files, saves, settings (`savagewind.ini`), shader caches and
`savagewind.log`. Hold **Option** while opening the app for the configuration screen (what `--config` is elsewhere).

## Recommended settings (MacBook Air / base M-series)

1080p fullscreen, Anti-aliasing **SMAA**, VSync **Fast**: a locked 60 on an M4. TAA costs ~5 ms a frame on Apple GPUs
today (about 51 fps at 1080p on the M4, against ~64 with SMAA, uncapped), and a window maximised on a Retina screen
renders at its full resolution (3420x2146 on a 13" Air) - use 1080p or an upscaler there. Anisotropic filtering costs
next to nothing.

## Differences from Windows / Linux

- **VSync**: MoltenVK presents with FIFO or IMMEDIATE only. Fast (the default) is FIFO with the fewest images (in sync,
  least delay); On is FIFO with one more image; Adaptive is On; Off is IMMEDIATE. On holds the display's rate exactly:
  there is no VRR for the game's window on macOS, so the cap under the refresh rate other platforms keep for G-SYNC /
  FreeSync is off here (`SAVAGEWIND_VSYNC_MARGIN=1` puts it back).
- **New pipelines**: MoltenVK has no graphics pipeline libraries, so a pipeline seen for the first time is a whole Metal
  compile (5-50 ms) instead of a link, and its draws are skipped until it is built: a material can appear a moment
  late the first time it is seen, once. The precompile and the pipeline cache cover what was seen before. Video >
  Asynchronous shaders Off makes those draws wait instead: a short hitch the first time rather than a late material.
- **HDR**: Auto is SDR (macOS reports any display with EDR headroom as HDR, SDR panels included); Video > HDR On turns
  HDR10 on for XDR displays.
- **Performance overlay**: CPU, memory and GPU load / memory come from Mach and IOAccelerator; temperatures are not
  shown (Apple Silicon has them only behind private interfaces).
- **Upscalers**: FSR 1 and FSR 3 (built in). DLSS and FSR 4 are Windows-only, as on Linux.

What the renderer does differently on MoltenVK (`g_vk.moltenvk`, `runtime/nvn/vulkan`): a texture bound to two colour
slots is attached once (Apple GPUs are tilers like Mali: both slots stored per tile in no set order), the fragment
outputs take the attachment's view format (Metal refuses a float output into an integer target), integer targets never
blend, and pipelines of the same shader module are built one at a time (MoltenVK converts a module unguarded).

## Testing on a Mac: what to report

The game version (1.9.0), the `test` commit, your Mac (chip, memory, macOS version), the settings, and
`savagewind.log`. `tools/test_simd.py --simd asm` (and `--locals`) checks the AArch64 FP/SIMD path on your machine in
about a minute.
