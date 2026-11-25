# Runtime profile for `PGO=use`

`runtime.profdata` is the profile that `make PGO=use` reads (change it with `PGO_PROFILE=<file>`). It only covers the
runtime (`runtime/`), never the generated game library, and holds execution counters keyed by function names
(`file.c:function`): no game data, no paths.

Made with clang 23.1.3 (MSYS2 UCRT64; the `compiler-rt` and `llvm-tools` packages give `PGO=gen` and `llvm-profdata`),
after Alpha 6, from the Alpha 7 work (`main` at b948239f: all the PR blocks of November 18 merged). One training run of
the instrumented build (`make PGO=gen`): `tools/perf_routes.py run --sample 0 --no-trace` over the kakariko, field,
hateno, korok and rito routes at 1920x1080 with TAA and high shadows, so the profile is of the TAA path the game is
played in. The file is about 870 KB; its size follows the number of counters in the code, not the play time.

Refreshed because the Alpha 6 profile had gone stale, which Fayberr found and measured in PR #889: a profile trained on
Alpha 6 gave +10.9% at the plateau over the one shipped then (FXAA, 5700X3D + 3080 Ti). Against that stale profile, the
compiler flagged out-of-date data in 47 files; against this one, in 1 function. It was trained first on the performance block alone (0e638cdc plus those PRs) and again the same night once every block was merged, on the same routes; the numbers below are from the first one. Measured on a 5600X + RTX 3060
(`make dist`'s `PGO=use LTO=thin` both, 3 rotated rounds, TAA, high shadows, 1920x1080, routes NOT in the training):
plateau 127.5 -> 136.3 fps (+6.9%, 1% low 98.5 -> 107.1), castle 126.4 -> 135.6 fps (+7.3%, 1% low 96.5 -> 104.1).

The profile before that (from `test` at 04aa642, the same routes) measured on a 5700X3D + 3080 Ti against the plain
build: with `PGO=use LTO=thin` plateau 1080p +33%, castle 1080p +23%, plateau 1440p +27%, castle 1440p +19%; PGO alone
+28 / +18 / +21 / +17%. The one before it (AA off, older code) gave +1..+10% on the same routes and a merge of the two
was worse than the new one alone: regenerate instead of merging.

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
