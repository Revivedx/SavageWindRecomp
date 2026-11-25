# CPU option mask for the render thread's draw preparation

`SAVAGEWIND_VK_CPU_OPTS` is one mask of switches for the work the render thread does before each draw. With no
variable the mask is 3312 (`CPU_OPTS_DEFAULT`: every bit but 512, the material cache off); `SAVAGEWIND_VK_CPU_OPTS=0`
runs the code as it was before the mask. The bits are independent of each other.

Measured on Beta 1 (5600X + RTX 3060, 1920x1080, Original preset, the same build with 0 and 3312 interleaved over two
rounds): Plateau 143.4 -> 152.0 fps (+6.0%), Castle 136.4 -> 142.8 (+4.7%), Hateno 153.8 -> 157.0 (+2.1%), Rito Village
108.1 -> 110.1 (+1.9%). Screenshots of both showed nothing new.

## The mask

The low two bits are the fragment material cache mode (0 off, 1 on, 2 on with the cross-program sharing, see
`docs/PERF-FRAGMENT-MATERIAL-CACHE.md`). They are also set by `SAVAGEWIND_VK_STAGE_REUSE=0..2`, which wins over the
mask's low bits. The mode 3 is treated as 0.

| bit | what |
| --- | --- |
| 16 | throughput-bound descriptor-set hash (`mum64`, `set_finish`) |
| 32 | texture change detection from the write watch (`textures.c`) |
| 64 | cache-line guest copy table, prefetches ahead of a draw's constant buffers and textures |
| 128 | block bitmap in front of the storing-shader range search, static scratch for the descriptor writes of a set |
| 512 | far shadow cascades drawn every second frame. **Known bad: the ground and rocks flicker while panning** (the stale cascade is read with this frame's matrices). Leave off |
| 1024 | per-thread dispatch table for indirect calls (`runtime/dispatch.c`) |
| 2048 | O(1) command buffer unlink (`nvn/cmdbuf.c`) |

Every bit except 512 plus the cache mode 2 is `3314`. (With 512 it would be `3826`.) `CPU_OPTS_VALID` is `3 | 3824`.

The live command `cpuopts [mask]` (needs `SAVAGEWIND_CONTROL` and `SAVAGEWIND_VK_CPU_BENCH=1`) reports and changes the
mask; `tools/swr_ab.py` uses it to A/B the bits inside one process. It keeps the cache mode the game started with in
every window, so the cache is not a factor in its numbers.

## Tests

Each bit has a host test that needs no game files: `tools/test_tex_write_watch.py` (32), `tools/test_guest_copy_line.py`
(64), `tools/test_gw_filter.py` (128), `tools/test_shadow_budget.py` (512), `tools/test_dispatch_cache.py` (1024),
`tools/test_cb_registry.py` (2048), `tools/test_vk_stage_reuse.py` (the cache mode and the mask parsing) and
`tools/test_ab_play.py` (`swr_ab.py`). The set hash (16) is covered by the differential cases of the stage reuse test.

## PGO

The runtime's profile (`tools/pgo/runtime.profdata`) is keyed by function and control flow. The functions these bits
change (`stage_set`, `set_finish`, `draw_`, the dispatcher and others) lose their profile data until it is retrained on
code that has them (`tools/pgo/README.md`), and compile without PGO in the meantime. Measure a PGO build only with a
profile trained on the same tree.
