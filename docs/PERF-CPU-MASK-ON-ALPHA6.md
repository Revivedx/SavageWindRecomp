# CPU option mask on top of Alpha 6 and the material cache

Branch `perf/r4-on-alpha6`, base `perf/alpha6-stage-reuse` (Alpha 6, PR 815 fix, PR 858, material cache, 64 MB VRAM
blocks, FSR3). Everything here is behind `SAVAGEWIND_VK_CPU_OPTS` (a mask) and defaults to off.

## The mask

The low two bits are Alpha 6's material cache mode (0 off, 1 on, 2 on with the cross-program sharing). They are also
set by `SAVAGEWIND_VK_STAGE_REUSE=0..2`, which wins over the mask's low bits. The mode 3 is treated as 0.

| bit | what | origin |
| --- | --- | --- |
| 16 | throughput-bound descriptor-set hash (`mum64`, `set_finish`) | R1 |
| 32 | texture change detection from the write watch (`textures.c`) | R1 |
| 64 | cache-line guest copy table, prefetches ahead of a draw's constant buffers and textures | R4 |
| 128 | block bitmap in front of the storing-shader range search, static scratch for the descriptor writes of a set | R4 |
| 512 | far shadow cascades drawn every second frame. **Known bad: the ground and rocks flicker while panning** (confirmed 2026-11-18, gone with it off). Leave off | R4 |
| 1024 | per-thread dispatch table for indirect calls (`runtime/dispatch.c`) | R4 |
| 2048 | O(1) command buffer unlink (`nvn/cmdbuf.c`) | R4 |

The mask to play with: every bit except 512, `3824 - 512 = 3312`, plus the cache mode 2 gives `3314`. (With 512 it would be `3826`.)

`CPU_OPTS_VALID` is `3 | 3824`. The live command `cpuopts [mask]` (needs `SAVAGEWIND_CONTROL` and
`SAVAGEWIND_VK_CPU_BENCH=1`) reports and changes it; `tools/stage_reuse_bench.py` and `tools/swr_ab.py` use it.
`swr_ab.py` keeps the cache mode the game started with in every window, so it is not a factor.

## What was left out

The older branch `perf/r4-render-microopts` had bits 1, 2, 4, 8 and 256: its own fragment preparation experiments.
Alpha 6's material cache covers the same ground and is already tuned, so they are not carried over. The commits
04aa642..cb202ef of that branch are in Alpha 6 already (PRs 762 and 763) and were skipped, not re-applied.

## PGO

`tools/pgo/runtime.profdata` is the same in Alpha 6 and here, but the Alpha 6 play build used a fresh profile
(`build/hunt/pgo-refresh/fresh.profdata`, trained on the Alpha 6 source). The functions changed by the bits above have
no profile data in it and compile without PGO until the profile is retrained on this tree.
