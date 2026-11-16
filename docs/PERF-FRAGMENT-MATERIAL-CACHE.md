# Experimental fragment-material preparation cache

Ordinary fragment shaders can share a material while being paired with different
vertex programs. Repeating fragment descriptor preparation for those draws costs
CPU time even when the existing descriptor-set cache later finds a match.
This cache reuses the prepared fragment descriptor and its dynamic offsets,
keyed by the fragment shader and descriptor layout across vertex programs.

The change is opt-in and leaves `stage_set` itself unchanged. Set
`SAVAGEWIND_VK_STAGE_REUSE=2` before launch to try it. The default, 0, calls the
original preparation path. Mode 1 retains a conservative reference that requires
the same program and all buffer ranges; it is for comparison, not the proposed
material strategy. Vertex and compute preparation still use `stage_set`.
`SAVAGEWIND_VK_NO_SET_CACHE` disables this cache as well as the existing cache.

For a developer session only, enabling both the control endpoint and
`SAVAGEWIND_VK_CPU_BENCH=1` registers `cpuopts [0-2]` for mode switching and
reuse counters. This is not required for normal play. The mode defaults off
pending wider validation; the wrapper's diagnostic counters remain in this draft.

## Validity and lifetime

Sixteen direct-mapped entries retain the resolved descriptor and dynamic-offset
order. A hit requires matching shader info, layout, submission sequence,
descriptor-pool generation, guest-copy generation, uniform-update serial and
GPU-write generation. It compares only the uniform ranges used by that stage,
along with compiler constants, resolved image descriptors, texture scales,
viewport scale and tint. In-place reboot already advances the GPU-write and
guest-copy generations, invalidating entries before they can be reused.

All preceding texture resolution, uploads, feedback handling, viewport and
shadow decisions still run. Storage/global-memory bindings, unknown binding
kinds and constant-buffer patch paths fall back to the original preparation.
The cache allocates no new GPU resources and does not extend descriptor lifetime
past its existing submission and pool generations.

## Existing native measurements

Game 1.9.0 on Windows, Ryzen 7 5700X3D, RTX 3080 Ti, NVIDIA 621.24. Alpha 6 play
source `6eda961` (release plus PR824/858), with the cache patch and the existing
shipped PGO profile, `RELEASE=1 PGO=use LTO=thin`, Clang 23.1.4. Settings:
2560x1440, upscaler off, FXAA on, high shadows, medium draw distance, render
scale 100%, dynamic resolution off, uncapped, VSync and FG off.

One process per scene, three rotated forward/reverse rounds, twelve seconds
per mode per round. Full captures, samplers and per-draw timers were off;
whole-list timings and frame traces were enabled. Fresh disposable test saves,
separate data roots and muted copied settings. Medians of three windows:

| Scene | Mode 0 FPS | Mode 2 FPS | Change | Mode 0 render CPU | Mode 2 render CPU | Mode 0 p99 | Mode 2 p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Plateau | 100.20 | 106.97 | +6.75% | 9.386 ms | 8.783 ms | 15.270 ms | 13.559 ms |
| Kakariko | 110.41 | 118.68 | +7.49% | 7.787 ms | 7.333 ms | 14.277 ms | 12.544 ms |

These are prototype mode 0 versus mode 2, not a direct comparison with a
separately built unmodified executable. Paired median render-CPU savings were
0.395 ms and 0.410 ms, lower in all six paired windows. Subtracting independent
medians gives a different result. Mode 1 medians were 103.17 and 116.39 FPS;
the refinement over that earlier strategy saves only about 0.17 ms paired
render CPU in each scene. Preparation reuse is a known approach, not a new
claim that the refinement alone accounts for the entire FPS gain.

Cloud lighting and wind varied despite fixed hour/weather settings. Screenshots
were inspected for geometry, material and HUD corruption with no obvious new
problem; there is no exact-frame or pixel-equality claim. The current upstream
`test` port has not been benchmarked. These results must not be added to the
separate PGO refresh measurements or treated as universal gains.

## Validation and remaining work

The original native PGO/ThinLTO build passed its loader health check and matching
game-library ID check. The port on upstream `test` passes Linux ARM64 production
object compilation for `exec.c` and `launch_flags_table.c` (Clang 18), the
ASan/UBSan differential test and existing feedback-alias checks:

```sh
python3 tools/test_vk_stage_reuse.py
python3 tools/test_feedback_alias.py
python3 tools/gen_launch_flags.py --check
```

The test extracts the actual C preparation, descriptor-cache and reuse functions;
it checks cross-program hits, used/unused uniform ranges, independent generation
invalidation, compiler code/constant changes, special-path fallback, dynamic
offsets and 2,000 differential cases, with the original set cache on and off.
The production compile retains the pre-existing unused `gmem_sparse_binds`
warning. The current-base branch has not had a new full native link or gameplay
run. Long sessions, broad driver coverage and full Vulkan validation remain
pending. No new benchmark was run when preparing this draft.
