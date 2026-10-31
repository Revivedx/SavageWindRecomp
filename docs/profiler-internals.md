# Profiler internals

How the profiler in `runtime/prof.c`, `runtime/sampler.c` and `tools/swprof.py` works, and the rules it relies on. For
using it, see [profiling.md](profiling.md). No game files, generated code or captures belong in issues or pull requests.

## Capture ownership and producer completion

Every capture has a generation number. A timed scope (`SW_ZONE`, `SW_ACC` and the renderer's manual draw spans) keeps
the generation it started in. When it ends, a scope from another generation is dropped, so a scope that crosses a stop
and a start cannot charge its time to the later capture. Recording checks admission again before it touches its cursor.

The recorder has 256 bounded thread slots. A thread-local key identifies a thread's slot. When the thread exits, its
destructor releases ownership and keeps that capture's final totals. Slots are not reused within a capture. The first
event of a thread takes the registry mutex; after that, a record only touches the calling thread's cursor and its
write-in-progress flag.

The ordering between producers and the writer, in `prof.c:enter` and `writer_main`:

1. A producer publishes `busy = 1` with sequential consistency, then checks its generation and admission, also with
   sequential consistency. It touches capture data only after the check succeeds.
2. Stop closes admission with a sequentially consistent store. The writer drains the GPU results, closes GPU admission,
   and then checks every busy flag under the registry mutex.
3. A producer that passed its check before admission closed finishes and publishes `busy = 0` before the writer reads
   its slot. A producer that arrives after the writer's scan fails the check and never touches capture data.

The flag covers the short recording operation, not the scope being measured, so a long scope cannot hold the writer.
Final accumulator totals are streamed as extra chunks, so they survive a full event pool and the exit of a worker.
If a frame flush fills the pool partway through, only stored sites are cleared. The remaining totals keep their
original frame for the writer; the new frame's addition that encountered the full pool is rejected as admission closes.
Thread ids are compacted for the file, and slots left over from earlier captures are omitted.

Normal shutdown and an in-place game reboot wait for the writer, so the file is finished before guest thread handles or
the game library are replaced. A full buffer requests a stop after leaving the recording critical section and logs the
reason. If the file cannot be opened, written or closed, no new path is published, and a partial file is removed.

## Linux sampling

Named threads register a lifetime slot even while capture is off. The sampler snapshots these slots before asking
the kernel for its running threads, then validates the slot generation and holds the lifetime mutex across
`pthread_sigqueue`. The thread's TLS destructor takes the same mutex before releasing its slot, so exit, detach and
join cannot reclaim pthread storage during signal delivery. Reused slots and unregistered threads are skipped.
The mutex is released before waiting for a signal response; the handler never takes it.

The handler for a sample must be async-signal-safe. glibc's `backtrace()` is not (it can take heap and loader locks), so
the Linux sampler does not unwind. It reserves `SIGRTMIN + 6` once for the lifetime of the process, and refuses to run
if that signal is already in use instead of replacing someone else's handler. `SIGPROF` is not touched.

Each request carries an id that is never reused. The handler claims the matching request atomically, reads the
interrupted program counter and the OS thread id, preserves `errno` and acknowledges. A request that times out is
cancelled only if the handler has not claimed it; otherwise the sender waits for the short handler to finish before it
reuses the storage. A cancelled request that arrives late does nothing. The handler stays installed after the capture
ends, so a signal that was blocked or delayed at stop time is harmless.

As a result, Linux samples hold a single program counter. Module classification and self time in the runtime work;
caller and inclusive time do not. Windows suspends the thread and unwinds with the x64 unwind data, with structured
exception handling around the unwind, and the module snapshot is bounded by its allocation if modules appear while it
is enumerated.

References: [glibc backtrace safety](https://www.sourceware.org/glibc/manual/latest/html_node/Backtraces.html),
[signal handler safety](https://man7.org/linux/man-pages/man7/signal-safety.7.html).

## Frame and GPU boundaries

Stop writes a closing frame mark separately from the event pool, so a one-frame capture has both boundaries even when
the pool is full. A manual stop closes the last partial frame at the time of the stop.

GPU admission stays open while the writer runs the backend drain. Under the renderer mutex, the backend submits pending
work and collects the timestamp results; CPU and GPU instrumentation for new work is off during the drain, and pending
events keep their generation so they are still written. Collecting does not wait on a query. Stopping can briefly
contend with rendering while the drain holds the renderer mutex.

GPU timestamps are aligned to the CPU clock by calibration: the device and an advertised host time domain are queried
together (the performance counter on Windows, `CLOCK_MONOTONIC` on Linux), and the host timestamp is correlated with
the profiler's cycle counter. The capture stores the driver's reported deviation and the bracket around the host read.
Without `VK_EXT_calibrated_timestamps`, the submission timestamp is used and the report labels the alignment as
estimated. Captures without calibration data show it as unknown. The metadata uses the existing counter record with bit
8 set, so it can be written during the drain; the record kind and the file version are unchanged.

Reference: [VK_EXT_calibrated_timestamps](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html).

## Offline attribution

`swprof` builds the nesting of zones first and then subtracts the direct children's intervals from each zone, which
gives its self-time intervals. Each interval is intersected with the frame boundaries, so a zone that spans several
frames is split between them. CPU and GPU intervals are clipped to a selected frame window, and an empty selection
reports no work. Counts include every operation that touches the window; times are clipped.

Accumulators are totals per thread and per observed frame. Their individual intervals are not stored, so they are not
split across frames. Scopes still open at stop are discarded rather than invented.
Accumulator self time subtracts direct children in the slash-separated hierarchy within each thread and frame.
To avoid counting accumulated work again as zone self time, its total is deducted proportionally from non-wait zone
self time on that same thread and frame, capped at the available zone time. Inclusive zone times are unchanged.
This is an estimate: ACC records carry neither intervals nor parent links, so unrelated zones sharing the frame can
lose self time too, and accumulator work beyond the recorded zone coverage remains separate. Wait zone self time is
preserved. Without complete frame boundaries the same estimate applies to the whole capture.

## Tests

```sh
make test-prof
```

This builds and runs the recorder test (`tools/test_prof.c`: the recorder and the sampler, several threads, a full
buffer), the report tests (`tools/test_swprof.py`) and the lifecycle tests (`tools/test_prof_lifecycle.py`, which drive
`tools/test_prof_lifecycle.c` as a subprocess). The lifecycle tests cover a scope that crosses a stop and a start, a
delayed `SIGPROF`, final totals of an exited worker, a one-frame capture, repeated captures with concurrent producers
and a failing writer. Macros that are off cost a branch; the finest zones can be removed with `-DSW_PROF_DISABLE`.

Not covered by the automated tests: GPU driver behaviour, the macOS sampler and Metal GPU timing, other GPU vendors,
and long captures.

## Points worth checking in review

- The admission and busy ordering, slot reuse, and the interaction of the thread-local destructor with a game reboot
- Delayed signal cancellation, the permanent ownership of the reserved signal, and the program-counter-only limit on Linux
- Vulkan query lifetime and reuse, renderer locking while stopping, and how frame boundaries are interpreted
- The calibration time domains, timestamp wrap, and the labelling and uncertainty of the fallback
- Self-time attribution for deep nesting, clipped boundaries and empty selections
