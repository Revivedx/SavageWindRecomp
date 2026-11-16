# Optional Alpha 6 FXAA runtime profile

`runtime-alpha6-fxaa.profdata` refreshes the runtime counters for an Alpha 6
FXAA workload. It is an alternative to the existing TAA-trained
`runtime.profdata`, which remains the default. Select it explicitly:

```sh
make -j16 B=build-fxaa RELEASE=1 PGO=use LTO=thin \
  PGO_PROFILE=tools/pgo/runtime-alpha6-fxaa.profdata build-fxaa/savagewind.exe
```

Use MSYS2 UCRT64 Clang 23.1.4, the compiler that wrote this profile. The profile
contains host-runtime execution counters and function identities, with value
profiling disabled. It contains no generated game library, game assets, saves
or raw profile captures. It has 4,880 functions, 43,176 blocks and is 869,024 bytes.
SHA256: `b4baef1db950079c31091e9eb2c647cab5dbbad8d5f401470991911213964959`.

## Provenance

Generated with `RELEASE=1 PGO=gen LTO=thin`, using source `6eda961` from the
Alpha 6 play integration branch: release `v0.6.0-alpha.6` plus PR824 and PR858.
It contains no fragment-material cache changes. This is not an exact-source
profile of the PR's current upstream `test` base. Edited functions may lose
profile coverage; it must not be described as training every current path.

Training used `tools/perf_routes.py` on field and Hateno at 2560x1440, FXAA on,
upscaler off, high shadows, medium draw distance, render scale 100%, dynamic
resolution off, VSync/FG off and uncapped frame rate. Each route requested
10 seconds of settling and 10 seconds of measurement. Two nonempty raw dumps
of 733,352 bytes were merged. A requested Korok route left an empty dump and
contributed no counters. Plateau and Kakariko were held out.

Only this merged, reusable profile is shipped. Do not merge it with the old
TAA profile and assume the result is better. TAA, other GPUs and long sessions
have not been compared for this refresh.

## Existing measurements

Windows, Ryzen 7 5700X3D, RTX 3080 Ti, NVIDIA 621.24, game 1.9.0. Same settings
as training. Both binaries used identical source `6eda961` and
`RELEASE=1 PGO=use LTO=thin`; the old profile versus this profile was the only
intended build change. Two valid 15-second windows per binary per scene,
A/B/B/A order. Means of those windows:

| Scene | Existing profile FPS | Fresh profile FPS | Change | Existing render CPU | Fresh render CPU | Existing p99 | Fresh p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Plateau | 104.82 | 116.19 | +10.85% | 8.97 ms | 8.02 ms | 14.54 ms | 12.72 ms |
| Kakariko | 115.84 | 117.13 | +1.12% | 7.33 ms | 6.83 ms | 13.86 ms | 13.39 ms |

Plateau repeats were 104.67 to 116.96 and 104.97 to 115.42 FPS. This is useful
on the tested Plateau workload, not a universal ten-percent gain. It measures a
profile refresh, not PGO versus an unoptimized build. Failed startup attempts
were excluded before measurement; all eight accepted windows reported no
unrelated game process. Full captures, samplers and per-draw timers were off.

The original-source native use build passed the loader health check (exit 73)
and had matching game-library ID `f63df6b1a6001541`. It reported one mismatched
function profile in an instrumentation-dependent path. The current-`test` PR
branch has not been benchmarked. These results must not be added to the
separately measured material-cache results or to an uncontrolled owner capture.

## Regeneration gotcha

The runtime periodically dumps counters every 3,000 presents, and
`SAVAGEWIND_EXIT_AT` explicitly dumps them before exit. A control-endpoint exit
can leave an empty raw file if the periodic dump never ran. Verify every input
is nonempty before `llvm-profdata merge`; use the matching compiler's
`llvm-profdata show --detailed-summary` to inspect the result.
