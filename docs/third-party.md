# Credits and third-party licenses

[Back to the README](../README.md)

- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai - the recompiler, the runtime and most of what
  this project is built on (MIT).
- [Ryujinx](https://github.com/Ryujinx/Ryujinx) (MIT) - its shader decoder was the reference for instruction field
  layouts.
- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh (MIT) - the generator passes that
  keep registers and flags in C locals (`tools/regs.py`, `tools/flags.py`, the flag helpers in `runtime/cpu.h`), and
  the rule for skipping a draw whose pipeline is still building and linking ready stages inline (`TOTK_PIPE_SKIP`;
  `runtime/nvn/vulkan/exec.c` one_shot_target, link_now; `third_party/totk-pc-recomp/LICENSE`).
- [envytools](https://github.com/envytools/envytools) - NVIDIA GPU documentation used for the shader translator.
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org)).
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) and the
  [Switch Loader](https://github.com/Adubbz/Ghidra-Switch-Loader) - used for reverse engineering (not bundled).
- Libraries: [SDL 3](https://libsdl.org) (zlib), [FFmpeg](https://ffmpeg.org) (LGPL),
  [Dear ImGui](https://github.com/ocornut/imgui) (MIT),
  [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (NVIDIA's own license; optional, Windows; `make dist` adds its `LICENSE.txt`),
  [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) (MIT),
  [SMAA](https://github.com/iryoku/smaa) (MIT), [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (MIT), [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) (MIT),
  [AMD FidelityFX SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4) (MIT: FSR 3 frame
  generation),
  [Fira Sans](https://github.com/mozilla/Fira) (SIL OFL 1.1), [Noto Sans Thai](https://github.com/notofonts/thai)
  (SIL OFL 1.1), [Noto Sans Hebrew](https://github.com/notofonts/hebrew) (SIL OFL 1.1), [stb_image / stb_image_write](https://github.com/nothings/stb)
  (public domain / MIT), [shaderc](https://github.com/google/shaderc)
  (Apache 2.0), [Zig](https://ziglang.org) (MIT), [Capstone](https://www.capstone-engine.org) (BSD),
  [Unicorn](https://www.unicorn-engine.org) (GPL 2.0), [python-lz4](https://github.com/python-lz4/python-lz4) (BSD).
- Colour LUTs from [FreshLUTs](https://freshluts.com), CC0 1.0, renamed: "Savage Wind" is
  [CINEMA 3](https://freshluts.com/luts/219) and "Dormant" is [CARIBE](https://freshluts.com/luts/217), both
  uploaded by Andy; "Ember" is [powerful cold](https://freshluts.com/luts/107), uploaded by sagi hershkovich
  (`third_party/luts/README.md`).

## What the Windows release bundles

The Windows release zip also bundles, unchanged unless noted, under their own licenses:

- FFmpeg 9.0.2 (libavcodec, libavformat, libavutil, libswscale), LGPL 2.1 or later: built from the official
  [source](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz) as LGPL only, without GPL or nonfree parts, by
  [tools/build_deps_windows.sh](../tools/build_deps_windows.sh). They are separate DLLs: you can replace them with your
  own build of the same version.
- From [MSYS2](https://packages.msys2.org) (UCRT64, sources there): SDL 3.4.18 (zlib), shaderc 2026.4 (Apache 2.0),
  libiconv 1.19 (LGPL 2.1 or later), the GCC 16.2 runtime (libgcc, libstdc++: GPL 3 with the GCC Runtime Library
  Exception) and winpthreads (MIT and BSD).
- Zig 0.17.0 (MIT; its bundled LLVM, Clang and LLD: Apache 2.0 with LLVM Exceptions; the MinGW-w64 headers and
  runtime: their own permissive licenses), trimmed to what the game code build uses.
- `swgen/`, this project's generator frozen with PyInstaller 6.22 (GPL 2.0 with the bootloader exception): Python
  3.14 (PSF License), Capstone 5.0.9 (BSD), [Unicorn 2.1.4](https://github.com/unicorn-engine/unicorn/tree/2.1.4)
  (GPL 2.0, source at that link) and python-lz4 4.4.5 (BSD).
- AMD FidelityFX SDK 1.1.4 (MIT, `FidelityFX-LICENSE.txt`), compiled into savagewind.exe: FSR 3 frame interpolation,
  optical flow and the Vulkan backend, from [its source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4)
  with two alignment fixes (`third_party/FidelityFX/README.md`) and shaders compiled by `tools/ffx_shaders.py`.
- The Fira Sans font, Medium and SemiBold (SIL OFL 1.1, `FiraSans-OFL.txt`), and the Noto Sans Thai and Noto Sans
  Hebrew fonts, Medium, for the menus in Thai and Hebrew (SIL OFL 1.1, `NotoSansThai-OFL.txt`,
  `NotoSansHebrew-OFL.txt`).
- Three colour LUTs in `colour-luts/` (CC0 1.0, from FreshLUTs: see "Credits" and `colour-luts/README.md`).
