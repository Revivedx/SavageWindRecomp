# Intel XeSS on Savage Wind (Windows, Vulkan)

`Video > Upscaler > Intel XeSS` runs Intel's XeSS-SR through `libxess.dll`, loaded at run time from
the game's folder (as AMD's FSR DLLs are, `vulkan/fsr_api.cpp`). The library ships nowhere: as with
NVIDIA DLSS, on a PC with an Intel graphics card the setup downloads it from Intel's own repository
(intel/xess v3.0.2, `runtime/xess_fetch.c`: size and SHA-256 checked, Intel's licence next to it), and
Savage Wind Configuration's game files page can download it again. On any other card a player can
still drop an official `libxess.dll` (XeSS 2.x, Windows x64) next to `savagewind.exe` and the entry
appears; without it everything is as before. Nothing of Intel's is committed here but the
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
- Dispatch runs on the recorder thread, as FSR 3's does (`exec.c xess_run`, a `rec_custom` job, after
  e0842cd's rework of the upscalers): the library records into the command buffer in the recorder's
  turn, so the thread that translates the game's command lists does not wait on it. Nothing may be
  queued from the recorder thread, so the input layout transitions, the full barrier before them and
  the library's own path call `vkCmd*` directly there (`rec_on_recorder`), and the per-frame image views
  are destroyed by the next dispatch instead of going to the translating thread's unlocked deferred
  list. `xessVKInit` is the one call that wants the device idle: it runs on a new output size only
  (`xess_output_changed`), not per frame.
- Layout contract: the game's images live in GENERAL; the inputs are transitioned to
  SHADER_READ_ONLY_OPTIMAL around the library's commands and back afterwards, the output stays in
  GENERAL (its documented requirement).
- Velocity scale is set per size (`xessSetVelocityScale(-0.5 w, +0.5 h)`): the game's vectors are
  clip-space NDC differences, XeSS takes pixels, its y pointing down.
- Where the shadow maps follow a temporal upscaler's factor and where Ultra Performance is offered
  (`output_scale.c`), `rt_xess` counts with FSR 3 and DLSS: the cascades keep as many texels per drawn
  pixel as native, and Ultra Performance waits for a 4K window.

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

## Testing notes

- `make -j8` builds and links `savagewind.exe` with the integration in; `python tools/i18n_check.py`:
  0 missing, 0 unused in every language; `git diff --check` clean.
- Run on an Intel Arc B580 (driver 101.8864, Vulkan 1.4.348) on Windows, `libxess.dll` 2.0.2 next to the
  executable, a hidden start with a warp into the Great Plateau: the log's `[xess] libxess.dll 2.0.2
  loaded`, `[xess] context made` and `[xess] initialized 2954x1662 -> 3840x2160` show it running, the
  scene goes through it (`[post] scene ... graded fsr3 -> ...`, the same path FSR 3 takes), and the game
  holds 116-123 fps at a 4K window with the game's dynamic resolution raising the render size. The
  overlay's "Upscaler: XeSS" line and the `[xess]` log lines report what actually runs.
- The dispatch runs on the recorder thread, as FSR 3's does (`exec.c xess_run`, a `rec_custom` job). The
  first version of it waited for the device every frame - one `vkDeviceWaitIdle` and a drained recorder -
  which cost most of the gain: the same view ran at 66-73 fps. The wait is now kept where the API asks
  for it, a new output size re-running `xessVKInit` (`xess_output_changed`), and nowhere else.
- One frame when entering the world falls back to FSR 1 while that frame's motion vectors are not made
  yet: upstream's existing behaviour (`fsr3_scene`'s "no vectors this frame"), not XeSS's.
- Not tested: a Radeon or GeForce GPU (XeSS's DP4a path), FSR 3 or FSR 4 in the same run, a window
  smaller than 4K (where the quality mode's own factor applies instead of the dynamic resolution), and a
  resize that re-runs `xessVKInit` in the middle of a session.
