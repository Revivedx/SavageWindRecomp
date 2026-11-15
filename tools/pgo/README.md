# Runtime profile for `PGO=use`

`runtime.profdata` is the profile that `make PGO=use` reads (change it with `PGO_PROFILE=<file>`). It only covers the
runtime (`runtime/`), never the generated game library, and holds execution counters keyed by function names
(`file.c:function`): no game data, no paths.

Made with clang 23.1.4 (MSYS2 UCRT64), from `test` at 04aa642. One training run of the instrumented build
(`make PGO=gen`): `tools/perf_routes.py run` over the kakariko, field, hateno, korok and rito routes at 1920x1080 with
TAA and high shadows (about 9 minutes), so the profile is of the TAA path the game is played in. The file is about
680 KB; its size follows the number of counters in the code, not the play time.

Measured on a 5700X3D + 3080 Ti (3 rotated rounds, TAA, high shadows, routes NOT in the training), against the plain
build: with `make dist`'s `PGO=use LTO=thin` plateau 1080p +33%, castle 1080p +23%, plateau 1440p +27%, castle 1440p +19%;
PGO alone +28 / +18 / +21 / +17%. The previous profile (AA off, older code) gave +1..+10% on the same routes and a merge of
the two was worse than the new one alone: regenerate instead of merging.

## Limits

- A profile only fits the compiler that wrote it. With another clang, `PGO=use` can fail with an "unsupported
  instrumentation profile format version" error: make a new one (below).
- It goes stale as the runtime changes. A function that was edited since loses its data and is built as usual; everything else keeps working. Regenerating every so often keeps the gain.

## Making a new profile

```
make PGO=gen B=build-gen RELEASE=1 build-gen/savagewind.exe
# run build-gen/savagewind.exe with LLVM_PROFILE_FILE=<dir>/rt-%p.profraw and play for a while (TAA on: the profile
# should match the settings the game is played with), or script it:
#   LLVM_PROFILE_FILE=<dir>/rt-%p.profraw python tools/perf_routes.py run --exe build-gen/savagewind.exe --routes kakariko field hateno korok rito ...
# The profile is written every 3000 frames and at a scripted SAVAGEWIND_EXIT_AT, so closing the window is fine.
llvm-profdata merge -o tools/pgo/runtime.profdata <dir>/*.profraw      # add the old file to the list to keep its data
make PGO=use B=build-pgo RELEASE=1 build-pgo/savagewind.exe
```
