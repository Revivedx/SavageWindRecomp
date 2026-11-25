# Render interpolation: any frame rate with the console's mechanics (Beta 2 plan)

Status: planned for Beta 2 (decided 2026-11-23). Nothing implemented yet. Beta 1 keeps the variable step (option A
below) and fixes mechanics one by one.

## The problem

BOTW has one clock. Each frame it updates everything (actors, AI, Havok physics) and draws once. Nintendo built it
for 30 and 20 fps: `ksys::VFR` hands the game a frame length of 1 or 1.5 console frames, and most code multiplies by
it. Savage Wind's high frame rates feed it shorter frames (0.5 at 60 fps, 0.25 at 120). Most of the game copes, but
not all of it:

- Havok work that counts steps, not time: deactivation, solver microsteps, cloth substeps, the navigation world.
- Hard-coded 1/30 and ×30 constants.
- Per-frame thresholds that a quarter-length frame falls under.
- Frame counters that add 1 per update.
- One-update trigger windows.

Every fix so far (`vfr_ratio`, `phys_solver_steps`, `phys_tick*`, `kf_angle`, `arrow_fall_gravity`,
`demo_move_arrive`, `rail_point_radius`, the 30 fps holds in events) closes one case. A Ghidra inventory of 1.9.0
(2026-11-23, kept privately with the issue logs because it is derived from the game's code) lists hundreds more
candidates. That approach never reaches "no mechanic breaks at any frame rate".

## How current games do it

Engines such as Unity, Unreal and Source run two clocks:

- **The simulation clock.** Physics, and often all gameplay, runs at a fixed rate (50 or 60 Hz), the same on every PC.
- **The display clock.** The engine draws as fast as the machine allows. In each drawn frame it places every object
  between the last two simulation states (interpolation).

The game's rules never see the display rate, so no mechanic depends on it. PC ports built from decompiled N64 games
do the same with the original game. Ship of Harkinian, 2 Ship 2 Harkinian and Zelda64Recomp keep the game logic at
its native 20 fps and draw at 60, 144 or 240 with interpolated matrices.

## Plan for Savage Wind (option B)

The game runs at exactly 30 updates per second, as on the console: logic and physics untouched, no physics patches
needed. The display runs at any rate. Each displayed frame is really rendered (this is not frame generation), with the
camera, object matrices and bone palettes interpolated between the last two game states. FSR 3 frame generation stays
available on top.

Savage Wind can't add the second clock inside the game's engine, because it has no engine source. It adds it in the
GPU layer, where it already sees every draw:

- **Draw identity.** The motion-vector work (`docs/motion-vectors-spike.md`) already matches 93-99% of each frame's
  scene draws to the previous frame's.
- **Previous inputs.** It already keeps each draw's constant buffers one frame longer. Those constants hold the
  object matrices, the camera and the bone palettes.
- **An in-between frame** is the last frame's command lists replayed with those constants blended between the previous
  and the current state: slerp or decomposition for rotations, lerp for positions and palettes. Unmatched draws use
  the current state.

### Phases

1. **Prototype.** Hold the game at 30 and draw one interpolated frame between two game frames. Measure:
   - how many draws come out right;
   - what breaks (particles, terrain clipmap, water, shadows, UI);
   - GPU and CPU cost;
   - whether the translated Vulkan command buffers can be reused with new uniform data instead of re-translating.
2. **Split the loop.** Fixed 30 Hz game updates and free presentation, paced by `framepace.c`. The camera turns with
   the mouse and stick at the display rate, so turning doesn't feel 30 Hz.
3. **Close the cases.**
   - Unmatched draws, matched by a stronger tie-break: nearest previous object matrix.
   - Particles and GPU-made data: keep their previous versions, or draw them at the current state.
   - UI layouts.
   - Events and videos.
   - Loading screens.
4. **Ship it as the default.** The variable-step mode (option A) stays as an alternative until B covers everything.

### Expected cost

- **CPU.** The game logic and the command-list translation run 30 times a second instead of at the display rate.
  CPU-bound PCs, the usual case in the open world, should get more frames. If command buffers can't be reused, the
  gain shrinks to roughly today's frame rate, not lower.
- **GPU.** Every displayed frame is still drawn in full, plus the blending: about today's frame rate on GPU-bound PCs.
- **Latency.** Interpolation needs the next state, so the picture trails the game by up to one update (~33 ms).
  Turning the camera at the display rate hides most of it; Link reacts to buttons at the console's 30 Hz.

### Rejected alternatives

- **Fixed-step Havok only, with the rest at the display rate.** Game code reads and writes the physics state every
  frame. Frames with no physics step would leave Link and actors frozen or out of sync, and it fixes only the Havok
  family.
- **Simulation capped at 30 or 60 with FSR 3 frame generation above it.** The user rejected it: it caps the frame rate
  that is actually simulated and rendered.
- **Patching every mechanic for any frame length (option A).** It stays for Beta 1, but it can't guarantee "no broken
  mechanics".
