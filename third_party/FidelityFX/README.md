# AMD FidelityFX SDK 1.1.4 (subset)

From <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>, MIT (`LICENSE.txt`). Savage Wind uses
FSR 3's frame interpolation and optical flow (`runtime/nvn/vulkan/framegen.cpp`) and its upscaler
(`runtime/nvn/vulkan/fsr3.cpp`) on the Vulkan backend, driven directly, without the SDK's frame interpolation
swapchain or its ffx-api DLL layer.

What is here:

- `sdk/include/FidelityFX/host`: the SDK's public headers; `gpu`: the shader headers of frame interpolation, optical
  flow, the FSR 3 upscaler, SPD (which optical flow and the upscaler include) and FSR 1 (the upscaler's RCAS).
- `sdk/src/components/frameinterpolation`, `opticalflow`, `fsr3upscaler`: the three effects.
- `sdk/src/backends/vk/ffx_vk.cpp` and its `shaders/frameinterpolation`, `shaders/opticalflow`,
  `shaders/fsr3upscaler` (GLSL).
- `sdk/src/shared`: the SDK's assert, message and object-management helpers.
- `sdk/src/backends/shared/ffx_shader_blobs.h`: the declaration of the blob lookup, which `framegen.cpp` implements.
- `spirv/ffx_shaders_vk.h`: the shaders compiled to SPIR-V with their bindings, by `tools/ffx_shaders.py`
  (glslangValidator) in place of the SDK's Windows-only FidelityFX_SC.exe. Only the permutations Savage Wind uses.
  Regenerate after changing the GLSL:
  `python3 tools/ffx_shaders.py third_party/FidelityFX/sdk third_party/FidelityFX/spirv/ffx_shaders_vk.h`.
- `spirv/ffx_fsr3upscaler_vk.h`: the same for the FSR 3 upscaler, one permutation per pass (SDR input,
  low-resolution motion vectors without jitter, depth not inverted), in 32-bit and 16-bit floats (FFX_HALF, used where
  the SDK allows it and the device was made with shaderFloat16 and 16-bit storage: device.c `vk_shader_fp16`), and the
  history reprojected with a Catmull-Rom filter from 5 bilinear samples instead of Lanczos on 16 loads
  (`FFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5`, a Savage Wind addition to `ffx_fsr3upscaler_reproject.h`; the
  history texture is the upscale size). Together they took its passes from 3.47 to ~2.4 ms at 3840x2160 (RTX 3060;
  the accumulation 1.8 to 1.42 ms):
  `python3 tools/ffx_shaders.py third_party/FidelityFX/sdk third_party/FidelityFX/spirv/ffx_fsr3upscaler_vk.h
  glslangValidator --effects fu`.
- `ffx_compat.h`: force-included into the SDK's sources so they build with clang and gcc (MinGW-w64, Linux).

Changes to the SDK's files (marked "Savage Wind" in them):

- `sdk/src/backends/vk/ffx_vk.cpp`: the arrays the backend lays out in its scratch memory are aligned to 64 bytes
  instead of 4, and start 64-byte aligned after its context. With 4, clang's aligned SSE stores into them (`movdqa`)
  faulted; MSVC emits unaligned ones.
- `sdk/include/FidelityFX/host/ffx_types.h`: the contexts' default size is doubled outside Windows, where `wchar_t`
  (their resources' debug names) is 4 bytes instead of 2.
- `sdk/src/backends/vk/ffx_vk.cpp`: the global descriptor pool is created with all six of the sizes it lists. It passed
  five, leaving out the storage buffers, which the FSR 3 upscaler's passes allocate from it (the Vulkan validation
  layer reports it; NVIDIA's driver allocates them anyway).

## AMD FSR 4 (FidelityFX SDK 2.4.0, `api/`)

FSR 4 for Vulkan exists only as AMD's signed Windows DLLs. `api/` holds the SDK v2.4.0's ffx-api headers
(`api/include/ffx_api.h`, `ffx_api_types.h`, `vk/ffx_api_vk.h`, `upscalers/include/ffx_upscale.h`; MIT, see
`LICENSE.txt`) that `runtime/nvn/vulkan/fsr_api.cpp` is written against. The DLLs themselves
(`amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`, 36 MB) are not in the repository:
`python3 tools/fetch_fsr_dlls.py` downloads them (SHA-256 checked) into `build/fsr-dlls/` together with AMD's licence
text; `--dest <folder of savagewind.exe>` puts them where the game loads them from. `make dist` copies them into the
release when they are in `build/fsr-dlls/`. Their licence: binary redistribution with that notice reproduced, no
reverse engineering. The DLL also holds AMD's build of FSR 3.1 (3.1.6); `SAVAGEWIND_FSR3_API=1` has Upscaler FSR 3 use
it in place of the built-in one (comparisons).
