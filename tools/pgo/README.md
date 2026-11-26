# Runtime profile for `PGO=use`

`runtime.profdata` is the profile that `make PGO=use` reads (change it with `PGO_PROFILE=<file>`). It only covers the
runtime (`runtime/`), never the generated game library, and holds execution counters keyed by function names
(`file.c:function`): no game data, no paths.

Made with clang 23.1.3 (MSYS2 UCRT64; the `compiler-rt` and `llvm-tools` packages give `PGO=gen` and `llvm-profdata`)
on Beta 1's `main` with the CPU option mask of PR #989 at its default 3312 (2026-11-25), with `train.py`'s full run: 11 boots, the `play` config (the Original
preset, anti-aliasing off, console shadows, 2560x1440) at all seven `perf_routes.py` places and `taa1080` at four, each
boot through day, a pan, a fly-around, the pause menu and the map, dusk, night, rain, a thunderstorm, snow and dawn.
The file is about 1 MB; its size follows the number of counters in the code, not the play time.

Refreshed because the profile of November 18 had gone stale: 116 runtime commits since, and 19 files (the achievements
among them) had no profile data at all. Measured on a 5600X + RTX 3060 (`PGO=use LTO=thin` both, the same code,
1920x1080, Original preset, the two profiles interleaved over two rounds):

| Route | Old profile | New profile | Change |
| --- | ---: | ---: | ---: |
| Plateau | 150.3 fps | 164.7 fps | +9.6% |
| Castle | 141.5 fps | 154.1 fps | +8.9% |
| Rito Village | 113.5 fps | 126.2 fps | +11.2% |
| Hateno | 154.8 fps | 175.6 fps | +13.4% |

Then retrained the same way once PR #989 was in (its edits had taken the data of `draw_`, `stage_set` and others away):
against `main` before it with the profile above, each build with its own profile, interleaved, Plateau 166.6 -> 175.2
fps (+5.2%), Castle 151.8 -> 159.4 (+5.0%), Hateno 171.6 -> 180.4 (+5.1%), Rito Village 125.4 -> 127.7 (+1.8%).

Earlier profiles, for comparison: the November 18 one (trained on `main` at b948239f with `perf_routes.py` over five
routes, TAA) gave plateau +6.9% and castle +7.3% over the Alpha 6 one it replaced (PR #889, Fayberr's finding). The one
before it (from `test` at 04aa642) measured on a 5700X3D + 3080 Ti against the plain build: with `PGO=use LTO=thin`
plateau 1080p +33%, castle 1080p +23%, plateau 1440p +27%, castle 1440p +19%; PGO alone +28 / +18 / +21 / +17%. A merge
of an old and a new profile was worse than the new one alone: regenerate instead of merging.

## Limits

- A profile only fits the compiler that wrote it. With another clang, `PGO=use` can fail with an "unsupported
  instrumentation profile format version" error: make a new one (below).
- It goes stale as the runtime changes. A function that was edited since loses its data and is built as usual; everything else keeps working. Regenerating every so often keeps the gain. After a change to a hot function (anything in `runtime/nvn/vulkan/` that runs per
  draw), check the `PGO=use` build log for "profile data may be out of date" and retrain if it names more than a few functions.
- The instrumented build must have the same function structure as the normal one: the profile is keyed by a hash of the
  control flow, so `#ifdef SW_PGO_GENERATE` must not add or remove branches or loops in a function that is also profiled
  (`present_locked` calls `pgo_frame()` unconditionally for this reason; both builds have the same statements).

## Making a new profile

```
make PGO=gen B=build-gen RELEASE=1 build-gen/savagewind.exe       # (copy swgame.dll, swgame.dll.id and nvngx_dlss.dll next to the exe)
LLVM_PROFILE_FILE=<dir>/rt-%p.profraw python tools/pgo/train.py --exe build-gen/savagewind.exe --tree . \
    --data-root <own set-up data root> --save <folder with save_0> --ini <savagewind.ini> --out <runs dir> --profiles <dir> \
    --env SAVAGEWIND_VK_CPU_OPTS=<the mask the game is played with, if any>
llvm-profdata merge -o tools/pgo/runtime.profdata <dir>/*.profraw   # skip the near-empty files of a re-exec parent
make PGO=use B=build-pgo RELEASE=1 build-pgo/savagewind.exe                # read the log for "out of date" warnings
```

`train.py` boots the instrumented runtime once per place and config, plays a list of scenes over the control socket (day,
a camera pan, a fly-around that streams in new ground, the pause menu and the map, dusk, night, rain, a thunderstorm,
snow, dawn), and ends each boot with `exit`, which makes the runtime write its profile (`runtime/control.c` `pgo_dump`).
Two configs: `play` (the player's own settings at the main monitor's size, all `perf_routes.py` places) and `taa1080` (TAA at
1080p, high shadows, four places). A full run is about 65 minutes; `--quick` is a smoke test (a few seconds per scene,
`--routes plateau --configs play` for one boot).

The runtime appends a snapshot of the counters every 3000 presented frames and at the end, and resets them after each
write, so the snapshots add up to the run (without the reset, the early frames were counted again in every later
snapshot). On Windows a boot can re-exec itself when the guest address space is taken: the parent leaves a near-empty
profraw (skip it when merging) and `train.py` retries a boot once. Use a data root of its own (the
first boot compiles shaders for about two minutes, so give `--boot` 420) and a scratch copy of a save, never the player's.

For a quick profile without `train.py`, run `perf_routes.py` over the routes with the same `LLVM_PROFILE_FILE`:
`python tools/perf_routes.py run --exe build-gen/savagewind.exe --routes kakariko field hateno korok rito ...`. Regenerate
instead of merging a new profile with an old one: a merge of the two was worse than the new one alone.
