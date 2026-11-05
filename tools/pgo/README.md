# Runtime profile for `PGO=use`

`runtime.profdata` is the profile that `make PGO=use` reads (change it with `PGO_PROFILE=<file>`). It only covers the
runtime (`runtime/`), never the generated game library, and holds execution counters keyed by function names
(`file.c:function`): no game data, no paths.

Made with clang 23.1.4 (MSYS2 UCRT64), from `test` at 257b1f3 plus this change. It was merged from two training runs
of the instrumented build (`make PGO=gen`): a short scripted run, and about 14 minutes played by hand on a late-game
save (with TAA on). The file is about 660 KB. Playing longer does not make it
grow much, since its size follows the number of counters in the code, not the play time.

## Limits

- A profile only fits the compiler that wrote it. With another clang, `PGO=use` can fail with an "unsupported
  instrumentation profile format version" error: make a new one (below).
- It goes stale as the runtime changes. A function that was edited since loses its data and is built as usual; everything else keeps working. Regenerating every so often keeps the gain.

## Making a new profile

```
make PGO=gen B=build-gen RELEASE=1 build-gen/savagewind.exe
# run build-gen/savagewind.exe with LLVM_PROFILE_FILE=<dir>/rt-%p.profraw and play for a while.
# The profile is written every 3000 frames and at a scripted SAVAGEWIND_EXIT_AT, so closing the window is fine.
llvm-profdata merge -o tools/pgo/runtime.profdata <dir>/*.profraw      # add the old file to the list to keep its data
make PGO=use B=build-pgo RELEASE=1 build-pgo/savagewind.exe
```
