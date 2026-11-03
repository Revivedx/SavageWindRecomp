# Launcher game-file size

Settings and Game Files share one lazy background size calculation. Opening
either page starts it; later visits reuse its result. The size label reads
"Calculating..." while pending and "Unavailable" if enumeration or thread
creation fails. Partial totals are not displayed as complete measurements.

The cache owns a copy of the scan path. A generation counter cancels and rejects
old results after invalidation or a root/session change. Setup suspends scanning
and clears the result; deletion and DLC removal invalidate it. The next request
after the operation starts a new scan. Starting the game cancels the worker
and joins it; quitting ends it with the process. Cancellation is checked
between filesystem operations; it does not interrupt an OS call that is already
blocked.

The total is logical file length, limited to the same setup-created entries as
the deletion allowlist in `runtime/game_files.h`. Links are skipped. The size
is informational and does not authorize deletion. The cache lasts for one
launcher session and is refreshed by its setup/removal actions, without a
filesystem watcher for external edits.

- Windows reads file sizes and reparse attributes from native directory
  enumeration. Paths use UTF-8, wide APIs and extended-length absolute paths.
- Linux performs one directory-relative metadata lookup per entry and opens
  child directories without following symlinks.
- macOS retains portable traversal, now on the worker. Native macOS bulk
  enumeration and multi-worker Linux scanning are deferred.

## Verification

`make test-game-file-size test-file-size-cache` runs synthetic filesystem and
worker tests without game files or a GPU. Scanner tests cover exact totals,
empty trees, Unicode names/roots, paths over 260 characters, sizes over 4 GiB
and cancellation. POSIX checks include symlinks, unreadable directories, files
whose contents are unreadable, and deletion. Cache tests hold a scan while
servicing requests and verify reuse, invalidation, stale completed results,
root changes, suspension, incomplete results, cancellation, restart and thread
creation failure.

Verified 2026-10-30 against upstream `test` at `0dece28`:

- Full Linux ARM64 and Windows x64 runtime builds pass. The generated game-code
  library and its ABI were not changed or rebuilt.
- Scanner: 22 Linux checks and 15 Windows checks. Cache tests pass on both.
  Linux AddressSanitizer/UndefinedBehaviorSanitizer, ThreadSanitizer for the
  cache, and Clang static analysis pass. Portable scanner tests pass on Linux;
  a native macOS build/run remains unverified.
- Existing binding-capture and vibration tests pass on both hosts. Linux RomFS
  regression passes. All 12 language tables have zero missing/unused entries.
- Windows ACL fixtures verify that an unreadable directory reports an
  incomplete result and denied file-content access does not prevent measuring
  accessible metadata. A junction fixture preserves the expected total.
- The real Windows game folder returns 15,157,992,803 bytes in 126-145 ms,
  including console-process startup, over three runs. The Linux warmed
  41,067-file synthetic tree measures about 75 ms including process startup.
- Five diagnostic Windows launcher runs cover normal scanning, an artificial
  two-second worker delay, Game Files first, empty data and access denial. Each
  process performs one shared scan across repeated page switches. CPU UI
  construction remains below 4.5 ms, with pending/ready/unavailable states and
  25 screenshots checked from logs; representative screenshots inspected.
  An explicit worker-stop diagnostic cancels a 30-second delayed scan and
  exits normally without waiting for the delay.
- Linux launcher screenshot smoke passes with software Vulkan.

The UI diagnostics force page transitions/readiness without loading game code;
they measure UI construction, not full presented-frame or physical input
latency. Timing/page-selection instrumentation and artificial delays are only
in ignored verification copies, not the runtime sources committed here.
