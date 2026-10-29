# Per-object motion vectors: what BOTW's draws give us (spike, 2026-10-25)

A temporal upscaler (AMD FSR 3.1, NVIDIA DLSS, Intel XeSS) or frame generation needs, per pixel, where that pixel was
in the previous frame. Vectors from the depth buffer and the camera alone are right only for things that don't move by
themselves; Link, enemies, horses, NPCs, wind-swept grass, water and particles would smear. This spike measured what
Savage Wind's renderer sees of BOTW's scene draws, to choose how to make vectors for everything.

The idea being tested: almost everything that moves in BOTW moves because of the vertex shader's inputs (an object's
matrix, a bone palette, a time or wind value), not because its vertices are rewritten. If each scene draw can be given
the previous frame's inputs, a second copy of its vertex shader computes where each vertex was, and the difference is
the motion vector, with no knowledge of what the object is.

## Method

Instrumentation (`runtime/nvn/vulkan/mv_spike.c` plus one call in `exec.c` `draw_`, `SAVAGEWIND_MV_SPIKE=1`; a throwaway
commit, see "Reproducing") recorded the **scene draws** (depth or first color target in the RS_SCENE
scale group: the 3D scene's targets at the internal resolution) of two consecutive frames every 240 presented frames.
Each draw of the second frame was matched to one of the first by a **key**: vertex and fragment shader code addresses,
the addresses of the non-instanced vertex streams the vertex shader reads, the index buffer address, index count,
first index and base vertex; draws repeating a key in a frame were told apart by their order (ordinal). For each
matched draw it compared:

- each vertex shader constant buffer (hardware slot 3+): same guest address or not, same contents or not (hash of up
  to 64 KB);
- the non-instanced vertex streams' bytes (same key: rewritten by the CPU between the frames?);
- the instanced streams (divisor > 0): address, bytes, instance count;
- whether the vertex shader reads global memory (`uses_gmem`) or textures (`ntex`).

Run: the Alpha 3 build (5173d27) with only the runtime swapped, on the ROG Ally (RADV, Z1 Extreme), 1.8.2,
the owner's save (a scratch copy, `SAVAGEWIND_SAVE_DIR`), Great Plateau. Scripted input (`SAVAGEWIND_INPUT`): Continue,
then still, walking forward (`STICK_90`), a camera pan (`RSTICK_RIGHT`), walking sideways while panning, still again.

## Results

In the world from frame ~2880 (before that, the title and loading: 13 scene draws or none).

| Phase (frame pair) | Scene draws | Matched | Keys repeated in the frame | Instanced | VS reads textures | VS cbuf >= 2 KB |
|---|---|---|---|---|---|---|
| Still (3120) | 3114 | 98.4% | 1766 (57%) | 50 | 1603 (51%) | 240 |
| Walking (4080) | 3935 | 95.4% | 2472 (63%) | 82 | 1942 (49%) | 280 |
| Camera pan (4800) | 2359 | 93.0% | 1329 (56%) | 107 | 1395 (59%) | 250 |
| Walk + pan (5520) | 2637 | 93.1% | 1587 (60%) | 47 | 1331 (50%) | 235 |
| Still again (6480) | 2368 | 98.8% | 1405 (59%) | 116 | 1573 (66%) | 255 |

Constant buffers of the matched draws:

| Phase | cbuf slots | Same address | Moved | Moved, same contents | Draws with every cbuf at the same address | Draws with any cbuf contents changed |
|---|---|---|---|---|---|---|
| Still (3120) | 10097 | 31.3% | 6933 | 5102 (74%) | 8.3% | 54% |
| Walking (4080) | 11966 | 28.4% | 8573 | 1834 (21%) | 6.7% | 99% |
| Camera pan (4800) | 7840 | 23.4% | 6002 | 1534 (26%) | 11.5% | 99% |
| Walk + pan (5520) | 8208 | 23.6% | 6269 | 1510 (24%) | 10.3% | 99% |
| Still again (6480) | 8790 | 31.8% | 5992 | 2046 (34%) | 10.8% | 99% |

Every pair: **0** draws read global memory in the vertex shader; **0** draws had their (non-instanced) vertex data
rewritten between the frames; the cbuf slots that stay at one address and change contents are always the same 231
(per-frame globals). Instanced draws keep their instance stream address (45-116 of 47-117 draws) and instance count
(0-2 changed), but their instance **data** changes every frame for most of them (30-87 draws), standing still too.

The raw log lines are in the appendix.

## What this means

1. **Constant buffers are re-allocated every frame.** Only 23-32% of the slots keep their address, and three quarters
   of the moved ones (standing still) carry the same bytes as before: the game writes its constants into fresh memory
   each frame. "The previous frame's data at this address" doesn't exist; the previous inputs must be kept **per
   draw**: a copy of each scene draw's constant buffers, found again next frame by matching the draw.
2. **Matching draws by key works for 93-99% of them.** Standing still, 98.4-98.8%; moving, 92-95% (objects streaming
   in and out, LOD changes, culling). The unmatched rest (1-8%) is mostly a handful of shaders (the log's "unmatched"
   lists) and needs a fallback: camera-only reprojection, or the reactive mask.
3. **Over half the draws share their key with other draws in the same frame** (the same mesh drawn for several
   objects, told apart only by their constants). The ordinal is a weak identity: when culling adds or removes one of
   them, every later one would take its neighbour's previous constants, and get a wrong vector. These need a stronger
   tie-break, for example the nearest previous object matrix (the per-object constant slot is the one whose contents
   differ between the repeated draws).
4. **The constants carry the camera too.** Moving or panning, 99% of the draws had some constant change, so the
   per-object constants hold view-dependent matrices (or the game's camera block changes address with them). Running
   the vertex shader with the previous constants gives the whole motion, camera and object, in one go; the camera's
   own matrices are needed only for the unmatched fallback and for the jitter.
5. **Everything the vertex shaders read besides vertex streams and textures is in constant buffers.** No global memory
   reads, so bone palettes are constants too (the ~240 draws with a constant buffer of 2 KB or more, most of them one
   shader, `72cb2fb900`, 222 draws in every frame). Skinned characters need nothing beyond the constant copies.
6. **No CPU-built geometry in the scene passes.** No draw's vertex data changed between frames: no CPU skinning, no
   CPU-written cloth or particle vertices there. (Particles may come from instancing, or from passes outside the
   RS_SCENE targets, not checked.)
7. **Instance data is rewritten every frame** for most instanced draws (grass, foliage, effects), at the same address
   and with the same count. The previous instance data must be kept like the constants (a copy per draw), and its
   order may change between frames (open question below).
8. **About half the scene draws sample textures in the vertex shader** (terrain heights, grass and wind
   displacement...). Static textures need nothing; any that the game rewrites each frame (a displacement or interaction
   map) would need its previous version kept, or those draws marked reactive.

## Design this points to

- **Draw identity:** key = shaders, non-instanced vertex streams, index range (as measured) + a tie-break for repeated
  keys by the nearest previous per-object constants; unmatched draws fall back to camera reprojection.
- **Previous inputs:** Savage Wind already copies each draw's constant buffers per submission (`cbuf_info` ->
  `guest_copy`); keep last frame's copies alive one more frame and remember them by draw identity. Same for instanced
  streams.
- **Motion vertex shader:** a variant of each scene vertex shader from the translator (`maxwell/translate.c` emits GLSL
  reading `cbN[i][j]`): the position computed a second time from the previous-frame constants (and instance data),
  both clip positions passed to the fragment stage, the jitter removed from both.
- **Where the vectors go:** first a replay pass (the scene's opaque draws drawn again into a motion target, depth-tested
  against the scene depth: isolated, easy to check), later an extra output of the game's own geometry passes.
- **Masks:** blended / transparent draws and unmatched or texture-animated draws into the reactive and transparency
  masks.
- **Debug view:** a colour-coded vector overlay and a reprojection error measure (the previous frame warped by the
  vectors compared with this one).

## Open questions (next measurements)

1. **Instance order:** are a draw's instances in the same order frame to frame (only their data changing), or re-sorted
   / re-culled? Compare instance records between the two frames (count of equal records at equal and at other indices).
2. **Vertex shader textures:** which of the textures vertex shaders sample are render targets written in the same or
   the previous frame (GPU-made), and which are static?
3. **Repeated keys:** which constant slot differs between draws sharing a key, and does the nearest-matrix tie-break
   make the matching unambiguous (measure: matches whose tie-break disagrees with the ordinal)?
4. **Unmatched draws:** what the shaders in the "unmatched" lists are (they include some that are unmatched even when
   standing still: alternate-frame updates?).
5. **Passes outside RS_SCENE:** particles and effects drawn into other targets, and how the scene's transparent passes
   are ordered.
6. **Cost:** bytes of constants and instance data kept per frame (memory for one extra frame of copies).

## Answers from the implementation (TAA, desktop, Great Plateau)

Measured with TAA on (Video > Anti-aliasing), a reprojection error view (the previous frame warped by the vectors,
minus this one, x4) and the jitter off (`SAVAGEWIND_JITTER=0`), so only vector errors show.

- **Vectors:** the error is lowest at the vectors' own scale and sign (mean 0.39 against 0.53-0.57 at -1, 0 and 2);
  sky, far terrain, rocks and Link are near zero. What is left is thin grass blades and leaves uncovering what was
  behind them (no vector can say what was there; TAA's neighbourhood clamp is what handles it) and outlines.
- **Vertex shader textures (2):** ~1450 of ~2100 scene draws a frame have vertex shaders that sample textures; the
  only other buffer they read outside slots 3-16 is slot 1 (the shader's own compiler constants: static). Seven
  textures they read are drawn by the GPU every frame (a 512x512 RGBA8 linear target, a 256x256 RG8 one, 128-192
  RGBA8 / RGBA16 levels). Giving the previous frame's copy of the vertex shader those textures' previous contents (a
  copy before each frame's first pass into them, 8 a frame, 63% of the lookups) changed the error by nothing beyond
  noise, standing still (grass in the wind) and moving: the grass's motion comes from its constants, which the history
  already has. Not kept.
- **Instance data (1):** ~3 instanced draws a frame get a motion program, 1-2 of their streams (~2 KB) change from
  frame to frame. Not worth previous-frame vertex inputs (a second vertex binding and attribute set in every pipeline
  of those draws).
- **Matching (3):** the fingerprint is the first 12 floats of the draw's first 4 constant buffers after the camera
  block (slot 3, which every scene draw shares). With the camera block in it, no fingerprint equalled its match's
  while the camera moved and the search never stopped early; without it, recording costs 0.68 ms of CPU a frame for
  ~2300 draws (was 0.90) and matches to another object (a world position jump over 1 cm) are 5-9 a frame in a pan, 0
  standing still.
- **Cost (6):** ~2.3 MB of constants a frame go through the history, ~240 KB copied (the rest shared within a
  submission), a few KB of instance data; the history slots (5, one per frame in flight) start at 2 MB (0.2-0.5 MB
  used) and grow when a frame fills one. Uncapped at 4K: 81.6 fps with TAA, 94.1 with SMAA; the GPU side is ~0.6 ms a frame (the geometry pass's doubled vertex work and the motion target),
  the rest is CPU (recording, the motion program lookup, push constants).
- **Shader compiles:** the motion variants go into the precompile key log like any other; with the cache warm, a TAA
  run compiles nothing in play and world entry peaks at 67-108 ms (75-191 without TAA).

## TAA: FSR 3.1's temporal reconstruction at 1x

The first TAA (one fragment pass: Catmull-Rom history in a fixed 1.25 sigma YCoCg box, one blend weight for every
pixel) flickered on foliage at about twice AMD FSR 3's rate. It is now the reconstruction of FSR 3.1's upscaler
(the vendored SDK's shaders were the reference, not copied), at the scene's own size, as seven compute passes
(`shaders.c` `taa_*.comp`, `exec.c` `mvec_taa`):

- **Dilation:** each pixel's vector and depth are the nearest surface's in its 3x3, so silhouettes move with the
  foreground.
- **Disocclusion from depth:** this frame's nearest depths are scattered (atomic min) to where they were last frame;
  a surface behind what landed in its place was hidden. No previous depth buffer is kept.
- **Per-pixel accumulation:** 1/3 more a frame, 0 on a shading change, two frames of the new image alone after a
  disocclusion. The blend is the new sample's own weight (how close its jittered samples fall to the pixel) against
  it: about 1/23 on a converged still pixel.
- **Shading change:** this frame's 5-luma neighbourhood against last frame's as sorted sets (jitter and sub-pixel
  motion reorder values but don't change them), over three sizes.
- **Anti-flicker:** a pixel whose luma matches an older frame better than the last one (it oscillates with the
  jitter) and thin features (luma ridges filling no 2x2 quadrant) keep their history from the clamp.
- **Clamp:** an ellipsoid around the 3x3's Gaussian mean, 1-3 standard deviations by confidence, motion and distance.
- **Around it:** the game's own textures in the jittered passes get a -1 mip bias (`log2(render / output) - 1` under
  an upscaler); Video > Sharpening is RCAS after TAA, as under FSR 3 (CAS sharpened the residual flicker up); the
  jitter steps once per presented frame (paused, the game updates twice a frame and only half the phases were used).

Measured at 4K on an RTX 3080 Ti, still view, flicker (temporal second difference of luma, 75th percentile of the
region) against AMD's own FSR 3 at the window's size (the same algorithm in AMD's code: the FSR 3 upscaler that builds
on this, with its `fsr3-native` test mode) in the same run, interleaved: trees 0.23-0.29 against 0.27-0.30, grass
0.40-0.47 against 0.39-0.48, far field 0.23-0.25 against 0.23-0.25. GPU time 2.0 ms a frame (FSR 3 native: 2.3), plus
0.09 ms of RCAS; the old TAA was 0.6.
`SAVAGEWIND_TAA_VIEW=<frame>:<1-7>` shows its masks (disocclusion, accumulation, shading change, instability, locks,
motion divergence, no vector); `SAVAGEWIND_MIP_BIAS=<levels>` replaces the bias.

## Reproducing

The instrumentation (`SAVAGEWIND_MV_SPIKE=1`, `[mv]` log lines) was a throwaway commit, not part of this branch: the
runtime built in the Linux container (`make B=build/linux/x86_64/game DEPS=... build/linux/x86_64/game/savagewind`), run
with a scripted input such as
`900:A:8,1400:A:8,1900:A:8,3500:STICK_90:900,4500:RSTICK_RIGHT:600,5200:STICK_0:700,5200:RSTICK_LEFT:700`.

## Appendix: raw output (in-world pairs)

```
[mv] frames 2880/2881: scene draws 3110 -> 3110; matched 3061 (98.4%), unmatched 49, repeated keys 1763; instanced 46; vs global memory 0, vs textures 1600, vs cbuf >= 2 KB 240
[mv]   cbuf slots (matched draws) 10076: same address 3157 (31.3%; of those contents changed 231), moved 6919 (same contents 5096); draws with every cbuf at the same address 253 (8.3%), with any cbuf contents changed 1637
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 46, instance data changed 30, instance count changed 0
[mv]   unmatched: vs 72db649600 x20 vs 72db64a200 x20 vs 72db64f200 x9 vs 72db64f200 x-9 vs 72db64f200 x9 vs 72db64f200 x-9
[mv]   cbuf moved: vs 72da979900 x305 vs 72d9f3f100 x303 vs 72da1dc600 x292 vs 72db5d8500 x252 vs 72db39fe00 x223 vs 72db38ad00 x127
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x13 vs 727f9fac00 x2 vs 727f63c700 x2 vs 727f638c00 x2 vs 727f9f8400 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da5c8b00 x4 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da70d900 x2 vs 72dac0fb00 x1
[mv] frames 3120/3121: scene draws 3114 -> 3114; matched 3065 (98.4%), unmatched 49, repeated keys 1766; instanced 50; vs global memory 0, vs textures 1603, vs cbuf >= 2 KB 240
[mv]   cbuf slots (matched draws) 10097: same address 3164 (31.3%; of those contents changed 231), moved 6933 (same contents 5102); draws with every cbuf at the same address 253 (8.3%), with any cbuf contents changed 1641
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 50, instance data changed 34, instance count changed 0
[mv]   unmatched: vs 72db649600 x20 vs 72db64a200 x20 vs 72db64f200 x9 vs 72db64f200 x-9 vs 72db64f200 x9 vs 72db64f200 x-9
[mv]   cbuf moved: vs 72da979900 x305 vs 72d9f3f100 x303 vs 72da1dc600 x292 vs 72db5d8500 x252 vs 72db39fe00 x223 vs 72db38ad00 x127
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x13 vs 726ebde700 x3 vs 727f63c700 x3 vs 727f9fac00 x2 vs 727f638c00 x2 vs 727f9f8400 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da5c8b00 x4 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da70d900 x2 vs 72dac0fb00 x1
[mv] frames 3360/3361: scene draws 3114 -> 3114; matched 3065 (98.4%), unmatched 49, repeated keys 1765; instanced 50; vs global memory 0, vs textures 1603, vs cbuf >= 2 KB 240
[mv]   cbuf slots (matched draws) 10099: same address 3165 (31.3%; of those contents changed 231), moved 6934 (same contents 5100); draws with every cbuf at the same address 253 (8.3%), with any cbuf contents changed 1641
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 50, instance data changed 33, instance count changed 0
[mv]   unmatched: vs 72db649600 x20 vs 72db64a200 x20 vs 72db64f200 x9 vs 72db64f200 x-9 vs 72db64f200 x9 vs 72db64f200 x-9
[mv]   cbuf moved: vs 72da979900 x305 vs 72d9f3f100 x303 vs 72da1dc600 x292 vs 72db5d8500 x252 vs 72db39fe00 x223 vs 72db38ad00 x127
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x13 vs 726ebde700 x4 vs 727f63c700 x3 vs 727f638c00 x2 vs 727f684900 x2 vs 727fcedb00 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da5c8b00 x4 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da70d900 x2 vs 72dac0fb00 x1
[mv] frames 3600/3601: scene draws 2969 -> 2964; matched 2898 (97.8%), unmatched 66, repeated keys 1685; instanced 53; vs global memory 0, vs textures 1455, vs cbuf >= 2 KB 240
[mv]   cbuf slots (matched draws) 9353: same address 2360 (25.2%; of those contents changed 231), moved 6993 (same contents 1946); draws with every cbuf at the same address 253 (8.7%), with any cbuf contents changed 2867
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 53, instance data changed 35, instance count changed 0
[mv]   unmatched: vs 72db649600 x20 vs 72db64a200 x20 vs 72db64f200 x9 vs 72db5a2200 x8 vs 72db38ad00 x4 vs 72db5d8500 x3
[mv]   cbuf moved: vs 72d9f3f100 x303 vs 72da979900 x302 vs 72da1dc600 x289 vs 72db5d8500 x209 vs 72db39fe00 x200 vs 72db5a2200 x112
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x12 vs 726ebde700 x6 vs 727f63c700 x3 vs 727f638c00 x2 vs 727f9f8400 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da5c8b00 x4 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da70d900 x2 vs 72dac0fb00 x1
[mv] frames 3840/3841: scene draws 1338 -> 1309; matched 1237 (94.5%), unmatched 72, repeated keys 812; instanced 24; vs global memory 0, vs textures 888, vs cbuf >= 2 KB 240
[mv]   cbuf slots (matched draws) 5119: same address 1052 (20.6%; of those contents changed 231), moved 4067 (same contents 1885); draws with every cbuf at the same address 253 (20.5%), with any cbuf contents changed 1207
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 24, instance data changed 15, instance count changed 2
[mv]   unmatched: vs 72db5a2200 x26 vs 72db387900 x16 vs 72db594500 x5 vs 72db649600 x3 vs 72da1dc600 x3 vs 72db64f200 x3
[mv]   cbuf moved: vs 72db2cdd00 x195 vs 72db212900 x112 vs 72db1f8a00 x79 vs 72db1e5200 x69 vs 72db5d8500 x61 vs 72db1d8600 x43
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 727fb21d00 x2 vs 727f63c700 x2 vs 727f638c00 x2 vs 727f684900 x2 vs 727f9fac00 x1 vs 727f63f800 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da5c8b00 x4 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da70d900 x2 vs 72dac0fb00 x1
[mv] frames 4080/4081: scene draws 3935 -> 3935; matched 3755 (95.4%), unmatched 180, repeated keys 2472; instanced 82; vs global memory 0, vs textures 1942, vs cbuf >= 2 KB 280
[mv]   cbuf slots (matched draws) 11966: same address 3393 (28.4%; of those contents changed 231), moved 8573 (same contents 1834); draws with every cbuf at the same address 253 (6.7%), with any cbuf contents changed 3725
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 81, instance data changed 47, instance count changed 0
[mv]   unmatched: vs 72db5a2200 x37 vs 72db649600 x26 vs 72db64a200 x26 vs 72db38ad00 x22 vs 72db64f200 x13 vs 72db5d8500 x12
[mv]   cbuf moved: vs 72da979900 x423 vs 72da1dc600 x417 vs 72d9f3f100 x405 vs 72db39fe00 x210 vs 72db5d8500 x175 vs 72da155700 x163
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f169e00 x10 vs 726ebde700 x8 vs 727fbbf600 x7 vs 726f1c4000 x5 vs 726eca6c00 x4 vs 727f63c700 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72d9cf5d00 x8 vs 72dacabb00 x6 vs 72da7ddb00 x6 vs 72da14e800 x4 vs 72d9da8400 x4
[mv] frames 4320/4321: scene draws 3795 -> 3794; matched 3596 (94.8%), unmatched 198, repeated keys 2438; instanced 78; vs global memory 0, vs textures 1863, vs cbuf >= 2 KB 236
[mv]   cbuf slots (matched draws) 11584: same address 3367 (29.1%; of those contents changed 231), moved 8217 (same contents 1906); draws with every cbuf at the same address 253 (7.0%), with any cbuf contents changed 3566
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 78, instance data changed 46, instance count changed 0
[mv]   unmatched: vs 72db38ad00 x30 vs 72db649600 x25 vs 72db64a200 x25 vs 72db387900 x23 vs 72db5a2200 x21 vs 72db5d8500 x19
[mv]   cbuf moved: vs 72da979900 x420 vs 72da1dc600 x418 vs 72d9f3f100 x392 vs 72db39fe00 x244 vs 72da155700 x190 vs 72db5d8500 x136
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f169e00 x12 vs 726ebde700 x10 vs 726eca6c00 x6 vs 727fc67700 x4 vs 727f63c700 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da5c8b00 x2 vs 72dac0fb00 x1 vs 72d9d17300 x1
[mv] frames 4560/4561: scene draws 2509 -> 2490; matched 2316 (93.0%), unmatched 174, repeated keys 1434; instanced 42; vs global memory 0, vs textures 1337, vs cbuf >= 2 KB 235
[mv]   cbuf slots (matched draws) 7883: same address 1907 (24.2%; of those contents changed 231), moved 5976 (same contents 1466); draws with every cbuf at the same address 253 (10.9%), with any cbuf contents changed 2285
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 40, instance data changed 29, instance count changed 0
[mv]   unmatched: vs 72db387900 x43 vs 72d9f3f100 x27 vs 72db38ad00 x14 vs 72db649600 x12 vs 72db64a200 x12 vs 72db5a2200 x12
[mv]   cbuf moved: vs 72db39fe00 x212 vs 72d9f3f100 x211 vs 72da979900 x180 vs 72da1dc600 x153 vs 72db3ad600 x113 vs 72db38ad00 x112
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x13 vs 726f640000 x3 vs 727f63c700 x2 vs 727f638c00 x2 vs 727f684900 x2 vs 727fcd9500 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da5c8b00 x2 vs 72dac0fb00 x1 vs 72d9d17300 x1
[mv] frames 4800/4801: scene draws 2399 -> 2359; matched 2194 (93.0%), unmatched 165, repeated keys 1329; instanced 107; vs global memory 0, vs textures 1395, vs cbuf >= 2 KB 250
[mv]   cbuf slots (matched draws) 7840: same address 1838 (23.4%; of those contents changed 231), moved 6002 (same contents 1534); draws with every cbuf at the same address 253 (11.5%), with any cbuf contents changed 2163
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 103, instance data changed 87, instance count changed 2
[mv]   unmatched: vs 72db387900 x53 vs 72db38ad00 x25 vs 72db5d8500 x16 vs 72db649600 x9 vs 72db64a200 x9 vs 72d9f3f100 x8
[mv]   cbuf moved: vs 72db39fe00 x185 vs 72d9f3f100 x148 vs 72da979900 x130 vs 72db38ad00 x114 vs 72da1dc600 x106 vs 72db3ad600 x101
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726f1c4000 x11 vs 726f640000 x4 vs 727f684900 x2 vs 727fcd9500 x1 vs 727f9fac00 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2 vs 72da77a900 x2
[mv] frames 5040/5041: scene draws 2252 -> 2270; matched 2103 (92.6%), unmatched 167, repeated keys 1347; instanced 102; vs global memory 0, vs textures 1486, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 7735: same address 1714 (22.2%; of those contents changed 231), moved 6021 (same contents 1627); draws with every cbuf at the same address 253 (12.0%), with any cbuf contents changed 2072
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 102, instance data changed 81, instance count changed 0
[mv]   unmatched: vs 72db38ad00 x48 vs 72db387900 x43 vs 72db5d8500 x12 vs 72db649600 x11 vs 72db64a200 x11 vs 72db64f200 x7
[mv]   cbuf moved: vs 72db39fe00 x180 vs 72db5d8500 x142 vs 72d9f3f100 x110 vs 72db38ad00 x98 vs 72da979900 x97 vs 72db3ad600 x94
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726f1c4000 x7 vs 726f640000 x2 vs 727f63c700 x2 vs 727f684900 x2 vs 727f9fac00 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 5280/5281: scene draws 2630 -> 2602; matched 2402 (92.3%), unmatched 200, repeated keys 1497; instanced 114; vs global memory 0, vs textures 1568, vs cbuf >= 2 KB 250
[mv]   cbuf slots (matched draws) 8703: same address 2919 (33.5%; of those contents changed 231), moved 5784 (same contents 946); draws with every cbuf at the same address 253 (10.5%), with any cbuf contents changed 2371
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 113, instance data changed 95, instance count changed 0
[mv]   unmatched: vs 72db387900 x45 vs 72db38ad00 x45 vs 72da979900 x14 vs 72db649600 x13 vs 72db64a200 x13 vs 72db5d8500 x12
[mv]   cbuf moved: vs 72db39fe00 x197 vs 72d9f3f100 x155 vs 72da979900 x149 vs 72db38ad00 x130 vs 72da1dc600 x117 vs 72db5d8500 x115
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726f1c4000 x13 vs 726f640000 x4 vs 727fc67700 x3 vs 727fa27e00 x2 vs 727fa29d00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2 vs 72da77a900 x2
[mv] frames 5520/5521: scene draws 2615 -> 2637; matched 2456 (93.1%), unmatched 181, repeated keys 1587; instanced 47; vs global memory 0, vs textures 1331, vs cbuf >= 2 KB 235
[mv]   cbuf slots (matched draws) 8208: same address 1939 (23.6%; of those contents changed 231), moved 6269 (same contents 1510); draws with every cbuf at the same address 253 (10.3%), with any cbuf contents changed 2425
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 45, instance data changed 30, instance count changed 1
[mv]   unmatched: vs 72db387900 x32 vs 72db38ad00 x23 vs 72da979900 x21 vs 72da1dc600 x15 vs 72db649600 x14 vs 72db64a200 x14
[mv]   cbuf moved: vs 72d9f3f100 x239 vs 72da1dc600 x216 vs 72db39fe00 x216 vs 72da979900 x208 vs 72db5d8500 x126 vs 72db3ad600 x118
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726f1c4000 x13 vs 726f640000 x5 vs 727f63c700 x2 vs 727f638c00 x2 vs 727f684900 x2 vs 727fcd9500 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da5c8b00 x2 vs 72dac0fb00 x1 vs 72d9d17300 x1
[mv] frames 5760/5761: scene draws 2987 -> 3032; matched 2819 (93.0%), unmatched 213, repeated keys 1857; instanced 54; vs global memory 0, vs textures 1441, vs cbuf >= 2 KB 235
[mv]   cbuf slots (matched draws) 9129: same address 2303 (25.2%; of those contents changed 231), moved 6826 (same contents 1583); draws with every cbuf at the same address 253 (9.0%), with any cbuf contents changed 2788
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 54, instance data changed 38, instance count changed 0
[mv]   unmatched: vs 72db38ad00 x38 vs 72db387900 x25 vs 72db649600 x19 vs 72db64a200 x19 vs 72da979900 x15 vs 72da1dc600 x13
[mv]   cbuf moved: vs 72da1dc600 x334 vs 72d9f3f100 x328 vs 72da979900 x293 vs 72db39fe00 x226 vs 72db5d8500 x125 vs 72db3ad600 x122
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726ebde700 x13 vs 726f1c4000 x9 vs 726f640000 x5 vs 727f684900 x2 vs 727fcd9500 x1 vs 727f9fac00 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72d9cc2e00 x2 vs 72da77a900 x2 vs 72da5c8b00 x2 vs 72dac0fb00 x1 vs 72d9d17300 x1
[mv] frames 6000/6001: scene draws 2365 -> 2368; matched 2337 (98.7%), unmatched 31, repeated keys 1403; instanced 116; vs global memory 0, vs textures 1573, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8778: same address 2794 (31.8%; of those contents changed 231), moved 5984 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2307
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 114, instance data changed 84, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 726e181000 x1 vs 726ec56700 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x8 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 6240/6241: scene draws 2368 -> 2369; matched 2340 (98.8%), unmatched 29, repeated keys 1406; instanced 117; vs global memory 0, vs textures 1574, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8796: same address 2800 (31.8%; of those contents changed 231), moved 5996 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2310
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 117, instance data changed 88, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x10 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f638c00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 6480/6481: scene draws 2368 -> 2368; matched 2339 (98.8%), unmatched 29, repeated keys 1405; instanced 116; vs global memory 0, vs textures 1573, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8790: same address 2798 (31.8%; of those contents changed 231), moved 5992 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2309
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 116, instance data changed 86, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x11 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f684900 x2 vs 727fcd9500 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 6720/6721: scene draws 2369 -> 2370; matched 2341 (98.8%), unmatched 29, repeated keys 1407; instanced 118; vs global memory 0, vs textures 1575, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8802: same address 2802 (31.8%; of those contents changed 231), moved 6000 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2311
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 118, instance data changed 89, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x13 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f684900 x2 vs 727fcd9500 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 6960/6961: scene draws 2372 -> 2373; matched 2344 (98.8%), unmatched 29, repeated keys 1410; instanced 121; vs global memory 0, vs textures 1577, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8818: same address 2808 (31.8%; of those contents changed 231), moved 6010 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2314
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 121, instance data changed 92, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x14 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f63c700 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 7200/7201: scene draws 2373 -> 2374; matched 2345 (98.8%), unmatched 29, repeated keys 1410; instanced 122; vs global memory 0, vs textures 1577, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8822: same address 2810 (31.9%; of those contents changed 231), moved 6012 (same contents 2044); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2315
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 122, instance data changed 78, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f684900 x2 vs 727fcd2b00 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 7440/7441: scene draws 2375 -> 2376; matched 2347 (98.8%), unmatched 29, repeated keys 1412; instanced 124; vs global memory 0, vs textures 1579, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8834: same address 2814 (31.9%; of those contents changed 231), moved 6020 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2317
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 124, instance data changed 80, instance count changed 1
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f63c700 x2 vs 727f638c00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 7680/7681: scene draws 2374 -> 2375; matched 2346 (98.8%), unmatched 29, repeated keys 1412; instanced 123; vs global memory 0, vs textures 1579, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8830: same address 2812 (31.8%; of those contents changed 231), moved 6018 (same contents 2044); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2316
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 123, instance data changed 93, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f638c00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 7920/7921: scene draws 2372 -> 2372; matched 2343 (98.8%), unmatched 29, repeated keys 1409; instanced 120; vs global memory 0, vs textures 1577, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8815: same address 2806 (31.8%; of those contents changed 231), moved 6009 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2313
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 120, instance data changed 91, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 8160/8161: scene draws 2373 -> 2374; matched 2345 (98.8%), unmatched 29, repeated keys 1411; instanced 122; vs global memory 0, vs textures 1578, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8824: same address 2810 (31.8%; of those contents changed 231), moved 6014 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2315
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 122, instance data changed 93, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f63c700 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 8400/8401: scene draws 2372 -> 2374; matched 2344 (98.7%), unmatched 30, repeated keys 1410; instanced 122; vs global memory 0, vs textures 1577, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8817: same address 2808 (31.8%; of those contents changed 231), moved 6009 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2314
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 121, instance data changed 92, instance count changed 2
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 726e181000 x1 vs 72cb2b5200 x-1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 8640/8641: scene draws 2376 -> 2378; matched 2346 (98.7%), unmatched 32, repeated keys 1414; instanced 126; vs global memory 0, vs textures 1580, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8831: same address 2815 (31.9%; of those contents changed 231), moved 6016 (same contents 2031); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2316
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 125, instance data changed 35, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72d9f3f100 x1 vs 72db39fe00 x1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x192 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x119 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f63c700 x2 vs 727f638c00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 8880/8881: scene draws 2371 -> 2373; matched 2343 (98.7%), unmatched 30, repeated keys 1410; instanced 121; vs global memory 0, vs textures 1577, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8813: same address 2806 (31.8%; of those contents changed 231), moved 6007 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2313
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 120, instance data changed 90, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 726e181000 x1 vs 72cb2b5200 x-1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f684900 x2 vs 727fcd9500 x1
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 9120/9121: scene draws 2372 -> 2372; matched 2343 (98.8%), unmatched 29, repeated keys 1409; instanced 120; vs global memory 0, vs textures 1578, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8817: same address 2806 (31.8%; of those contents changed 231), moved 6011 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2313
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 120, instance data changed 92, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f638c00 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 9360/9361: scene draws 2375 -> 2376; matched 2347 (98.8%), unmatched 29, repeated keys 1412; instanced 124; vs global memory 0, vs textures 1581, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8836: same address 2814 (31.8%; of those contents changed 231), moved 6022 (same contents 2046); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2317
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 124, instance data changed 94, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x193 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f63c700 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 9600/9601: scene draws 2371 -> 2371; matched 2342 (98.8%), unmatched 29, repeated keys 1408; instanced 120; vs global memory 0, vs textures 1576, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8810: same address 2806 (31.9%; of those contents changed 231), moved 6004 (same contents 2032); draws with every cbuf at the same address 253 (10.8%), with any cbuf contents changed 2312
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 120, instance data changed 92, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x192 vs 72db5d8500 x189 vs 72da979900 x121 vs 72d9f3f100 x120 vs 72db38ad00 x103 vs 72da155700 x91
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x60 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727fcd9500 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
[mv] frames 9840/9841: scene draws 2383 -> 2383; matched 2354 (98.8%), unmatched 29, repeated keys 1422; instanced 117; vs global memory 0, vs textures 1589, vs cbuf >= 2 KB 255
[mv]   cbuf slots (matched draws) 8872: same address 2832 (31.9%; of those contents changed 1), moved 6040 (same contents 4153); draws with every cbuf at the same address 253 (10.7%), with any cbuf contents changed 1357
[mv]   vertex streams rewritten between the frames (same key): 0 draws; instanced draws: same instance stream address 117, instance data changed 90, instance count changed 0
[mv]   unmatched: vs 72db649600 x10 vs 72db64a200 x10 vs 72db64f200 x8 vs 72cb2b5200 x1 vs 72cb2b5200 x-1 vs 72cb2b5200 x1
[mv]   cbuf moved: vs 72db39fe00 x194 vs 72db5d8500 x189 vs 72da979900 x122 vs 72d9f3f100 x120 vs 72db38ad00 x104 vs 72db3ad600 x92
[mv]   vertex data rewritten:
[mv]   instance data changed: vs 726e181000 x59 vs 726ec56700 x15 vs 726f1c4000 x3 vs 726f640000 x2 vs 727f63c700 x2 vs 727f684900 x2
[mv]   global memory:
[mv]   cbuf >= 2 KB: vs 72cb2fb900 x222 vs 72da108d00 x10 vs 72da5c8b00 x4 vs 72da009200 x3 vs 72da11aa00 x3 vs 72d9cc2e00 x2
```
