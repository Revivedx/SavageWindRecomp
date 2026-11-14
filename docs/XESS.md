# Intel XeSS on Savage Wind (Windows, Vulkan)

`Video > Upscaler > Intel XeSS` runs Intel's XeSS-SR through `libxess.dll`, loaded at run time from
the game's folder (as AMD's FSR DLLs are, `vulkan/fsr_api.cpp`). The library ships nowhere: the
player drops an official `libxess.dll` (XeSS 2.x, Windows x64) next to `savagewind.exe` and the
entry appears; without it everything is as before. Nothing of Intel's is committed here but the
MIT-licensed API headers (`third_party/xess/xess.h`, `xess_vk.h`).

## How it runs

- `vulkan/xess.cpp` fetches the API with `GetProcAddress` and never links the DLL: a missing or
  incompatible one only means the menu entry stays away.
- Before the instance is made, its required instance extensions and lowest Vulkan API version are
  asked (`xessVKGetRequiredInstanceExtensions`); before the device is made, its device extensions
  (`VK_EXT_mutable_descriptor_type` on current builds) and its feature chain are enabled with the
  device's own (`device.c`).
- After the device is made, `xess_probe` creates the context. An old-driver warning still counts;
  an unsupported device or driver (no DP4a / SM 6.4) means the setting falls back to FSR 3 or FSR 1
  (`device.c`), and a run-time failure falls back the same way (`exec.c fsr3_scene`).
- XeSS runs the exact input set FSR 3 does (`fsr3.h Fsr3Dispatch`): the graded scene, the depth as
  R32F [0, 1] (not inverted), the per-object motion vectors (mvec.c) at the render size, the
  reactive mask, and the jittered camera (output_scale.c, Halton over the sequence length XeSS
  recommends, 8 x the upscale ratio squared). TAA and SMAA stand down while it runs, as with
  FSR 3 (it anti-aliases the scene itself).
- Layout contract: the game's images live in GENERAL; the inputs are transitioned to
  SHADER_READ_ONLY_OPTIMAL around the library's commands and back afterwards, the output stays in
  GENERAL (its documented requirement). Per-frame image views are made for the library and released
  deferred once the submission has passed.
- Velocity scale is set per size (`xessSetVelocityScale(-0.5 w, +0.5 h)`): the game's vectors are
  clip-space NDC differences, XeSS takes pixels, its y pointing down.

## Frame generation and low latency (XeFG / XeLL): not possible over Vulkan, shelved

The task named them, but their libraries cannot run here:

- `libxess_fg.dll` (XeSS-FG) exports only `xefgSwapChainD3D12*` functions: it is a D3D12 swapchain
  presentation hook. There is no Vulkan entry point of any kind.
- `libxell.dll` (XeLL) exports only `xellD3D12CreateContext` / `xellD3D12SetAppQueue` / `xellSleep`:
  the same, D3D12 only. (Its remaining exports are the D3D11/D3D12 XeSS host functions it bundles.)

XeSS-FG also requires XeLL integration per Intel's SDK docs. A Vulkan port would mean replacing
the whole presentation path with a D3D12 one - a different engine. If a later Intel SDK ships
Vulkan entry points for either, `xess.cpp` is the place they would plug into; the frame generation
the game has today (AMD FSR 3, `vulkan/framegen.cpp`) stays.

## Files

- `runtime/nvn/vulkan/xess.h` / `xess.cpp` - the integration (loading, probing, dispatch).
- `runtime/nvn/output_scale.c` (`rt_xess`, `upscaler_available`), `runtime/nvn/output_scale.h`.
- `runtime/nvn/vulkan/exec.c` - `fsr3_scene`'s XeSS branch (inputs, fallback), TAA/SMAA stand-down.
- `runtime/nvn/vulkan/device.c` - instance/device extension and feature plumbing, probe, settings
  fallback.
- `runtime/settings.h` / `settings.c` - `UPSCALE_XESS`, `upscaler = xess` in `savagewind.ini`.
- `runtime/ui/menu.mm`, `runtime/ui/game_options.mm` - the menu entries, overlay line.
- `runtime/ui/i18n/*.inc` - the texts in all menu languages.
- `Makefile` - `xess.o` build rule (`third_party/xess` on the include path).

## Testing notes (code-level only; no game was started for this)

- `make -j8` builds and links `savagewind.exe` with the integration in.
- `python tools/i18n_check.py`: 0 missing, 0 unused in every language.
- `git diff --check` clean.
- Runtime behaviour (a real `libxess.dll` on an Intel Arc B580: quality, fps, the overlay line) is
  untested so far; the overlay's "Upscaler: XeSS" line and the `[xess]` log lines report what runs.
