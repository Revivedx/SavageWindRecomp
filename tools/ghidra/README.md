# Ghidra queries

Savage Wind keeps one analyzed Ghidra project of the game's `main` module next to the repository
(`../ghidra/`, never inside it) and queries it from the command line with `gq.py`. The analysis runs
once; every query opens the project read-only, so nothing in it changes.

## Setup (once)

- Ghidra 12.1.2 with the [Switch Loader](https://github.com/Adubbz/Ghidra-Switch-Loader) extension
  built for that exact version (`SwitchLoader-1.6.1-...-Ghidra_12.1.2.zip`, unzipped into
  `<ghidra>/Ghidra/Extensions/`). Extensions only load in the Ghidra version they were built for.
- A JDK 21 or newer.
- Import and analyze the 1.9.0 `extracted/exefs/main`, then `sdk` and `subsdk0` the same way (about 22,
  6 and 3 minutes on 8 threads of a Ryzen 5 5600X):

  ```
  GHIDRA_HEADLESS_MAXMEM=12G <ghidra>/support/analyzeHeadless <repo>/../ghidra botw_1.9.0 \
      -import <repo>/extracted/exefs/main -max-cpu 8 -log <repo>/../ghidra/analyze190_main.log
  ```

  `gq.py` opens `botw_1.9.0` by default, the supported version. An older `botw_1.8.2` project can be
  kept next to it and queried with `SW_GHIDRA_NAME=botw_1.8.2`.

- Point `gq.py` at the installation, through the environment or a git-ignored
  `tools/ghidra/gq.local.json`:

  ```json
  {"GHIDRA_INSTALL_DIR": "E:/tools/ghidra_12.1.2_PUBLIC",
   "JAVA_HOME": "C:/Program Files/Eclipse Adoptium/jdk-21.0.12.101-hotspot"}
  ```

## Use

```
python tools/ghidra/gq.py summary
python tools/ghidra/gq.py decompile 0x70001234a0        # a runtime address of main works too
python tools/ghidra/gq.py xrefs nvnBootstrapLoader
python tools/ghidra/gq.py strings "Actor/.*bactorpack" 20
```

Each call starts Ghidra headless (about half a minute), so pass several addresses to one
`decompile` or `xrefs` call when possible. The project cannot be queried while the analysis that
writes it is still running.

The Switch Loader maps `main` at `0x7100000000`; the generator and runtime use `0x7000000000`.
`gq.py` moves runtime addresses of `main` (`0x7000000000..0x7003ffffff`) to Ghidra's base and says
so on stderr. Output is printed, not stored: it is derived from the game's code and must stay out
of the repository.

## Working rule

Every claim about what a function does cites its evidence: the address, the relevant pseudocode
or instructions, and the callers that support the reading. Names given to functions (in notes,
patches or the runtime) say where they came from (a string reference, a zeldaret 1.5.0 match, a
call pattern).
