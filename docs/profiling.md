# Profiling Savage Wind

SWR has one profiler. It is off until you ask for a capture, costs about one branch per marker while off, and writes
one file per capture. A tool (`tools/swprof.py`) turns that file into text, JSON or CSV. Nothing is sent anywhere and
no viewer is needed.

## Taking a capture

| How | What it does |
| --- | --- |
| `F6` in the game | Starts a capture; `F6` again stops it. After a full buffer stops it automatically, the next press only acknowledges the stop; the press after that starts a new capture. Ignored while a menu captures input or F6 is assigned to a game control. |
| `SAVAGEWIND_PROF=<first>[:<frames>]` | Starts at frame `<first>`; stops after `<frames>` frames (without a count: stays on until stopped). `SAVAGEWIND_GPUPROF_AT=<n>` still works and means `<n>:1`. |
| `prof start`, `prof stop`, `prof status` | The same, from the command layer (once it is merged). |

A capture also ends by itself when its buffer is full (`SAVAGEWIND_PROF_MB`, default 256, 8 to 4096). The file is
written in the background to `profile/swprof-<date>-<time>.swprof`. This logs `[prof] buffer full, stopping capture`;
there is no on-screen notification. The next F6 press only acknowledges that stop, and the press after that starts
a new capture once writing is finished. Stopping first closes CPU recording, then drains
pending GPU timestamps on the writer thread. A capture started while the last file is still
being written is refused; wait for `prof status` to say `idle`.

While a capture runs the game is slightly slower. Keep captures short (5 to 30 seconds) and take them at the moment
the problem happens.

Normal exit and an in-place game reboot finish the file. Force termination and `_exit` test hooks bypass this.
The buffer remains allocated for reuse, plus about 6 MiB of bounded thread slots. A scope crossing a capture boundary
is discarded; it cannot contribute to a later capture. Completed final accumulator totals are retained.

## Reading it

```
python3 tools/swprof.py                      # the newest capture in ./profile
python3 tools/swprof.py FILE --exe PATH      # name the runtime's functions (PATH = the binary that was running)
python3 tools/swprof.py FILE --frames 120:240 --top 40 --slow 25
python3 tools/swprof.py FILE --json out.json --csv frames.csv
```

The report has, in this order:

- **Frames**: rate, average, p50, p95, p99, max, how many were over 33 and 50 ms.
- **Top places by self time**: the zones and added-up work with the most time of their own, per frame.
- **GPU**: busy and idle time, then every pass, dispatch and copy by GPU time. GPU rows are aligned to the CPU clock,
  so a slow frame can be matched to what the GPU was doing. The report labels alignment as calibrated, estimated
  (submission-time fallback), or unknown for an older capture, and includes calibration uncertainty when available.
- **Sampler**: where each thread was, by stack sample: game code, the SWR runtime, the driver and system, or waiting
  (inside one of the wait zones). Then the runtime's hot functions by self time, by inclusive time, and the runtime
  functions that were calling into the driver or system. Without `--exe` the functions show as `runtime+0x...`.
- **Threads**: zone tree per thread, then the added-up work (`SW_ACC`) tree.
- **Counters** and the **slowest frames** with the places that took the most time in each (and the sample split).

All times are milliseconds per frame unless a heading says otherwise. The sampler takes one stack per thread per
millisecond (`SAVAGEWIND_PROF_SAMPLE=<microseconds>`, 100 or more, `0` = off).
This is a requested interval, not a guaranteed rate. The report shows the observed interval. Linux records one
interrupted program counter per sample, rather than a stack. Runtime caller/inclusive attribution requires Windows
stack samples; Linux can classify waits only where a completed wait zone covers the sample.

Zone self time is split at child and frame boundaries. A frame selection clips CPU/GPU intervals to that window;
an empty selection reports no work. Operation/call counts include operations touching the window, while their times
are clipped. Accumulators are assigned to the frame observed by that thread; they do not retain individual intervals.
Their self time is deducted proportionally from non-wait zone self time on the same thread and frame, up to the
available zone time. This prevents duplicate self-time totals, but the missing ACC intervals mean the enclosing zone
cannot be identified exactly; unrelated zones in that frame can also be reduced. Zone inclusive time is unchanged.
Accumulator self totals also appear in slow-frame causes and CSV cells, assigned by the frame number in each ACC
record rather than its later flush timestamp. Slash-separated accumulator children are subtracted within that frame.

The recompiled game code is deliberately one total. The tool never looks up or prints a game function, and the
sampler section ends with a reminder: do not put game addresses, game names or generated code into pull requests or
issues. Runtime function names are fine.

## Adding markers

Include `prof.h`. Pick the cheapest marker that answers the question:

| Marker | Use it for | Cost while capturing |
| --- | --- | --- |
| `SW_ZONE("name")` | A stretch of work that happens up to a few hundred times a frame. One event each. | one event |
| `SW_ACC("a/b")` | Work done thousands of times a frame (a draw's steps). Summed per thread and frame; `/` nests. | two clock reads, no event |
| `SW_TALLY("name")` | Only how often something happens (cache misses). | a counter bump |
| `SW_COUNT("name", n)` | A number now: draws, bytes, queue depth. | one event |
| `prof_site_dyn(name)` | A name only known at run time (a pass's size). Cached, copied. | once per name |

Steady-state literal markers do not allocate or take a mutex. First use registers a name and claims a bounded thread
slot; dynamic GPU labels format text, intern names under a mutex and may allocate. Names should be string literals; put the finest zones
behind `-DSW_PROF_DISABLE` if they sit in the hottest loops. This disables the SW_* macros, not direct recorder calls
or the renderer's manual span markers. Wait zones that should count as waiting in the sampler are
listed in `WAIT_ZONES` in `tools/swprof.py`.
The Vulkan semaphore wait is recorded as the `gpu_wait` interval zone so samples inside it count as waiting, including
Linux's single-PC samples. With runtime symbols and stack samples, `vk_wait_seq` also identifies waits whose zone
has not completed or began before capture. Older captures containing only a `gpu_wait` accumulator require that
stack-based fallback; an accumulator cannot locate a wait in time.

## Platforms

- **Windows**: the sampler suspends each guest thread, reads its context and unwinds with the x64 unwind data.
  GPU time comes from Vulkan timestamps, aligned with `VK_EXT_calibrated_timestamps` when the driver has it.
- **Linux**: the sampler reserves `SIGRTMIN + 6` if its disposition is still default, and sends tagged requests with
  `pthread_sigqueue`. The handler reads the interrupted PC from `ucontext`; it never unwinds or allocates. The handler
  stays installed for the process lifetime so blocked, delayed requests remain harmless after stopping. If that signal
  is already in use, sampling is skipped with a log message; zones and counters still work. SIGPROF is untouched.
- **macOS**: zones, added-up work and counters work; GPU timing (Metal) and the sampler are not there yet.

## Tests

On Linux, `make test-prof` builds the recorder/sampler and lifecycle harnesses, and runs report tests on synthetic
captures. It covers delayed signals, scopes spanning captures, final totals, repeated start/stop with concurrent
producers, normal exit, writer-open failure, the final frame boundary, GPU draining and interval attribution. It also
prints synthetic marker costs with recording off and on. These numbers are not gameplay overhead measurements.

See [profiler internals](profiler-internals.md) for the capture lifecycle, the thread ordering rules and the limits.

The old `SAVAGEWIND_VK_PROF`, `SAVAGEWIND_TIMELINE` and `SAVAGEWIND_SAMPLE` interfaces were removed. Use
`SAVAGEWIND_PROF`, `SAVAGEWIND_PROF_SAMPLE` and `swprof.py`. The single-frame `SAVAGEWIND_GPUPROF_AT=<n>` alias remains;
its older comma-separated frame list is not supported. FNPROF and the F10 player overlay remain separate.

## File format

`SWPROF01` version 1; `runtime/prof.c` has the layout in its header comment. The tool refuses files with another
magic or version.
Short chunk headers and payloads are rejected with a message naming the chunk and the expected and available byte
counts, including truncation on a complete record boundary. The command exits unsuccessfully and writes no reports.
