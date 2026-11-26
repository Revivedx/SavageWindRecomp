# The game's threads on more cores: measurements (Beta 1)

Demetter's phase 7 (his fork `native-28oct`, read as `refs/remotes/demetter/native-28oct`): BOTW's job pool scheduled by
the runtime instead of the Switch's three-core scheduler, then every game thread scheduled by the OS. Reimplemented on
branch `perf-7` (2026-11-25) and measured. Nothing of it is in `main`: on 4- and 6-core CPUs it did not pay.

## What was tried

1. **The job pool scheduled by the runtime** (`SAVAGEWIND_JOBS_NATIVE=1`, commits 89a184de and 9d456020). The entries
   of a pool's push, kick and run-and-wait and of a worker's run (four 1.9.0 sites, an optional group) go to
   `runtime/hle/jobs.c`: the pool's round list is the one queue, the job workers are OS threads off the emulated core
   tokens and run their share of each group through the group's own run-chunk, waits use the OS's address wait
   (`plat_wait_u32`: WaitOnAddress, futex, ulock). `SAVAGEWIND_JOBS_CHECK=1` checks every job ran exactly once.
2. **Every game thread scheduled by the OS** at three game cores (ad7892a7): `SAVAGEWIND_CORES=0`, with every sead heap
   made with its own lock (five 1.9.0 sites). The Switch runs the threads of one core one at a time, and BOTW shares
   unlocked heaps between such threads; with the OS scheduling them, each heap takes its lock.

Both gave the same game: on five routes, ~1.7 million job groups checked with 0 wrong, no fault.

## Measurements

Ryzen 5 5600X (6 cores) + RTX 3060, the same exe with the switch off and on, interleaved over two rounds;
`--potato` is `tools/perf_routes.py`'s 4-core PC without SMT at 720p.

| Change | 1080p | 4 cores, 720p |
| --- | --- | --- |
| Job pool by the runtime (spin 50) | within noise (Plateau 179.5 -> 178.7 fps, Castle 161.5 -> 163.9) | Plateau +2.4%, Castle +1.1% |
| Job pool by the runtime (spin 1000) | within noise | Plateau -2.0% |
| The OS schedules every game thread (with the job pool) | Kakariko +2.3%, Plateau and Hateno unchanged | Plateau -2.0%, Kakariko -2.3% |

On six cores the OS scheduling helps only where the game's thread limits (Kakariko), and on four cores the game's
threads, now free of the core tokens, compete with the render, recording and motion-vector threads for the cores.
Demetter's larger gains came at more game cores on an 8-core 7800X3D, which needs the game's per-core structures made
for every core (some fifteen commits of his fork) and the game's locks made a PC's (`InternalCriticalSection` and the
condition variables native: +3% at three cores in his numbers). Not pursued: at the CPUs most players have, the ceiling
is a few percent and the risk is the game's logic.

## Reproduce

Branch `perf-7`: `make gen` (the new sites and patches), then the build, and
`SAVAGEWIND_JOBS_NATIVE=1 [SAVAGEWIND_CORES=0] SAVAGEWIND_JOBS_CHECK=1` with `tools/perf_routes.py`.
