# Splitting draw preparation over threads: assessment

Question: can the work the `nvn-queue` thread does per draw be spread over two or three threads, so that the render
side drops from about 13 ms to the 5.5 ms a steady 165 fps needs?

Short answer: not as an incremental change. The draw path is the engine's state machine, not a pipeline of pure
steps, and the parts that are pure are small. Nothing here was benchmarked; the evidence is a zone profile and a
static global-access audit (`tools/audit_globals.py`). No runtime code changed.

## Where the render thread's time goes

Capture of 2884 frames, 4431 draws a frame, 14.411 ms average frame, `nvn-queue` about 13.4 ms busy (about 3 us a
draw). Zone self time per frame, profiler overhead included (the zones add up to about 14.8 ms):

| Zone | ms/frame | Calls/frame |
| --- | ---: | ---: |
| exec/draw/sets (descriptor sets) | 3.494 | 8862 |
| exec/draw/pre (draw preparation) | 2.679 | |
| exec self (list walk, state commands) | 2.244 | |
| exec/draw/tex (texture info) | 1.651 | |
| exec/draw self | 1.404 | |
| exec/draw/vtx (vertex and index staging) | 0.935 | |
| vk enqueue (recorder hand-off) | 0.840 | 16147 |
| exec/draw/key (pipeline key) | 0.681 | |
| tex/upload check | 0.570 | 467 |
| exec/draw/dyn (dynamic state) | 0.290 | |

The cost is flat: thousands of small decisions per frame, each a few hundred nanoseconds, spread over the whole draw
path. There is no kernel that dominates. Bulk data work (copies, hashing) is already below about 0.6 ms in total.

## What the audit measures

`tools/audit_globals.py` compiles a C file to LLVM IR at `-O0`, reads every load, store, atomic and memcpy that
touches a global (static locals included, constants excluded), and follows direct calls to build, per function, the
set of globals the function and everything below it reads and writes. Calls to functions outside the file and
indirect calls are listed but not followed. The closure includes cold paths (error handling, debug logging), so it
overstates what one draw really touches; it is the number a thread-safety argument would have to cover, and it is
also the number a refactor has to untangle.

Results for `runtime/nvn/vulkan/exec.c` (functions reached, globals loaded, globals stored):

| Root | Functions | Loaded | Stored |
| --- | ---: | ---: | ---: |
| `draw_` | 282 | 562 | 511 |
| `exec_list` | 305 | 597 | 559 |
| `scene_post` | 89 | 181 | 149 |
| `resolve_feedback` | 76 | 174 | 145 |
| `begin_pass` | 78 | 159 | 157 |
| `gw_check` | 79 | 145 | 110 |
| `stage_set_prepare` | 107 | 168 | 124 |
| `cbuf_info` | 74 | 99 | 72 |
| `texture_info` | 40 | 70 | 46 |
| `guest_fetch` | 41 | 72 | 48 |
| `set_finish` | 29 | 48 | 32 |
| `guest_copy` | 7 | 15 | 8 |
| `copy_dedupe_in` | 7 | 9 | 6 |
| `vk_transient_copy` | 6 | 8 | 5 |

`draw_` alone has 105 global loads and 79 stores in its own body, before any callee. The pieces that are small enough
to move cleanly (`guest_copy`, `copy_dedupe_in`, `vk_transient_copy`, `snapshots_invalidate`, `fg_draw`) are exactly
the ones that cost little.

Limits of the method: it is static, so it cannot prove that two draws never touch the same state at run time; it
tracks accesses through one function's GEP chains, so state reached through a pointer returned by a callee is only
seen as "address passed"; it says nothing about the other files (`textures.c`, `record.c`, the shader cache) that the
outside callees live in. It supports "this is entangled", not "this is safe".

## Candidate cuts

1. **Decision on one thread, emit on another.** The measured ceiling (`SAVAGEWIND_NULL_EMIT=1`, 2026-11-02) is +32%
   at 1440p with TAA and only +9% at 1080p with AA off, so the gain depends on the settings. Splitting needs the roughly
   100 shared mutable globals (pass state, the dynamic-state cache, the set cache, texture backends, transient chunks,
   depth twins, feedback state, HUD patches, gmem, readbacks, the recorder ring) assigned to one side or the other.
   Inside one draw there is also a circular dependency: fragment textures are resolved first because their scales feed
   the viewport and render-scale decision, which in turn feeds the pipeline key and set contents.
2. **Descriptor sets as a pipelined stage.** Sets are the largest zone (3.5 ms), but `texture_info` touches views,
   depth twins, feedback and the scene upscale state, the set cache is shared, and the hash cost is already attacked by
   the CPU option mask's bit 16 (#989) and the fragment material cache (#890). A thread would move a shrinking cost.
3. **Pure kernels on a worker.** memcpy of staged data is about 0.3 ms, texture hashing is already cut by the CPU option mask's
   bit 32 (#989), the write-watch poll is at most about 0.6 ms, `guest_copy` is a table lookup that does not parallelize.
   Together they cap at well under 1 ms and each adds hand-off latency.
4. **Speculative preparation with validation stamps.** Prepare draws ahead, validate against state stamps, redo on
   mismatch. Needs a shadow interpreter of the state machine, so the logic exists twice and has to stay identical.
5. **Parallel command lists or passes.** Passes share the same globals; the guest also reuses state across lists.
6. **Moving work to the recorder thread.** It already runs about 5.3 ms a frame plus 0.37 ms submit, so it has no
   room; it would become the new limit.

## Verdict

Splitting the draw path over threads is very large (weeks), uncertain, and cannot be tested without a fast machine and
the game. The prerequisite is mechanical but large: group the draw-path globals into a render-context struct (for
example per-pass state, set cache, texture backends, transient memory, recorder ring), so that a second context or a
hand-off boundary becomes expressible. Only after that can the decision/emit split be evaluated against its +32% (1440p,
TAA) or +9% (1080p, AA off) ceiling. Doing the refactor first also helps every other render item.

## Cheaper steps first

1. A runtime PGO profile trained on the current code (#889: +18..20% at the plateau and the castle with a fresh TAA
   profile, in an earlier measurement). It goes stale with every edit of a hot function.
2. The descriptor-set hash and the texture write watch (bits 16 and 32 of #989; estimated 1.0 to 1.5 ms together, not
   benchmarked).
3. Whole-draw reuse extending the fragment material cache (#890).
4. A draw budget for the shadow cascades (about 810 of 4400 draws are shadow cascades); it changes the picture, and the
   first version (bit 512 of #989) flickers.
5. The main thread's guest work (about an 8 ms floor).
6. The thread split only after the render-context refactor.

A steady 165 fps at the plateau and the castle at 1440p with TAA stays roughly a 15 to 20% outcome even with all of this.

## Reproduce

```
python3 tools/audit_globals.py --self-test
python3 tools/audit_globals.py --summary runtime/nvn/vulkan/exec.c draw_ stage_set_prepare guest_copy -- \
    -Ithird_party/vma -Iruntime -Iruntime/nvn -Iruntime/nvn/vulkan -I<SDL3 include dir>
```

Drop `--summary` to list the globals themselves (stored, loaded only, address passed, outside callees).

## Experiments on Beta 1 (2026-11-25)

5600X + RTX 3060, 1920x1080, Original preset, `tools/perf_routes.py`, the same exe with the switch off and on,
interleaved over two rounds.

**A set worker** (branch `perf-c`, `SAVAGEWIND_VK_SETW=1`): the render thread still decided every descriptor, and a
thread of its own made the constant-buffer copies (the 98% that `UpdateUniformBuffer` does not write during the list:
only 1.5-2% of the copies cover such a range), the sets' hash and cache, their allocation and update; the recorder bound
them in their turn. It gave the same image and never fell back, but it was slower: Plateau 162.7 -> 148.5 fps, Rito
Village 118.1 -> 109.3, Hateno 167.9 -> 154.3, Castle 160.8 -> 156.6. Both the render thread (5.9 -> 6.5 ms at the
Plateau) and the game's main thread (4.7 -> 5.3 ms) took longer. A worker that yields instead of spinning made no
difference. The hand-over is the cost: a draw's two sets are ~1.2 KB written by one core and read by another (~4 MB a
frame), more than the copies the worker took over (~1.4 MB a frame), and at Rito 96% of the sets were cache hits the
render thread made in a few hashes anyway.

As with the parallel recording of October (branch `heap-vram`, see `docs/parallel-draws.md` there): on a 6-core CPU,
moving per-draw work to another thread did not pay. What paid on the same day was making the render thread's own work
cheaper: a retrained PGO profile (+9 to +13%), the CPU option mask of #989 (+2 to +5%), and no locked add per
transient span (`vk_transient`, +1 to +7%).
