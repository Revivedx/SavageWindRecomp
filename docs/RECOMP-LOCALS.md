# Registers and flags in C locals (`gen.py --locals`)

`make RECOMP_LOCALS=1` (the generator's `--locals`; the default since Alpha 5, `RECOMP_LOCALS=0` turns it off) turns on two passes ported from
[TotK Recomp](https://github.com/valentderah/totk-pc-recomp) (MIT, commit f469c5a; `third_party/totk-pc-recomp/LICENSE`):

- `tools/regs.py`: each generated function copies its 6 most used X registers (or SP) into C locals at its entry and
  works on those. It stores the ones it changed (a forward "dirty" plan over the function's flow) before a call, an
  SVC, a trap, a patch site, a return or a tail exit, and loads them again after: after a call only X0-X18 and X30
  (AAPCS64: the callee keeps X19-X29 and SP), after an SVC, a patch site or a setjmp all of them.
- `tools/flags.py`: NZCV lives in a local `Flags F` (`runtime/cpu.h`). A compare of a known kind keeps its operands, so
  `cmp; b.lt` becomes one C compare; the packed form is computed only where something needs it.

Without the passes every guest instruction loads and stores the Cpu, and clang can keep nothing in host registers,
since every guest store may alias the Cpu. The switch is part of `GAMELIB_ID` (Makefile) and reaches the start
screen's build of the game code (`runtime/gamebuild.c`, `GAMELIB_GENARGS`): a library built one way is never loaded
for the other, and players set up again when it changes.

## Where Breath of the Wild differs from TotK, and what the passes do there

- **setjmp / longjmp.** `main` imports both; libjpeg and libpng in `sdk` use them. A call to the guest setjmp
  (`gen.py setjmp_call`) runs on the Cpu: every dirty register is stored before it, its C (the host setjmp, the guest
  setjmp call) is left on the Cpu, and every local is loaded again after it, on both returns. The HLE longjmp
  (`runtime/hle/setjmp.c`) writes X19-X30 and SP and never returns to its caller, so the trust below never sees it.
  The flags after a setjmp are a call's: read from `c->nzcv` if anything reads them.
- **Patch sites** (`tools/patches.py`). Their hand-written C calls the runtime with `c`, reads and writes any register
  (`c->sp -= 0x30`, `c->x[0] = ...`), returns, calls guest functions and jumps to labels. It runs on the Cpu: dirty
  registers are stored and the flags published before it, its text is left as it is (`regs.OPAQUE`), and every
  register and `F.nz` are loaded again after it and before each of its `goto`s.
- **Other runtime calls with `c`.** SVCs store before and reload everything after. BRK and undefined words store
  before (they do not return). `rt_dispatch` (blr, br, jump table misses, calls outside the module) is a call or a
  tail exit. The cold `<name>_r` copy and functions with an unresolved computed `br` (all-instruction fallback) keep
  everything in the Cpu, as do functions with FP/SIMD inline asm (`--simd asm`, arm64 hosts).
- **Callee-saved trust.** Runtime writes to a running thread's X19-X29/SP: `hle_longjmp` (above); full-Cpu save and
  restore around a guest call (`runtime/pc_cursor.c`, `runtime/hle/nvn_boot.c`), which puts back what it found;
  `runtime/hle/movie.c` and `runtime/nvn/cmdbuf.c` restore X0-X7 only. Kernel threads each have their own Cpu, there
  are no guest fibers, and `ExitThread` / the unwind at shutdown leave by longjmp without returning. `sdk`'s C++
  unwinder (`_Unwind_*`, `__cxa_throw`) has no callers in `main`. So the trust is on; `--no-callee-saved-trust`
  reloads X19-X29 and SP after every call too.
- **Flags after a call.** Unlike TotK Recomp's, a read that can see a callee's (or a patch site's) flags reads them
  from `c->nzcv` (`flags.Plan.load`): what the function does stays the same as without the passes there. A callee
  still does not publish its flags when it returns, so code that reads a callee's flags (no AAPCS64 code does) would
  see the older ones.
- **Spin waits.** Guest code polls memory with plain loads in a loop (`ldr; cmp; b.eq` back to the load) and counts on
  the hardware to see the other core's store. Without locals the loop's own stores to the Cpu kept clang from moving
  the load out of it; with locals the loop stores nothing, clang hoisted the load, and the game hung at boot in
  `f_7000ef62dc` (found by bisecting main's functions). Every backward branch now carries a compiler barrier
  (`asm volatile("" ::: "memory")`), which keeps loads inside loops and leaves the locals in registers.
- **Diagnostics.** The hang watch and crash dumps (`runtime/kernel.c`, `runtime/dispatch.c`) read other threads'
  registers from the Cpu, which can now be older than the locals.

## Tests

- `tools/test_locals.py`: random hand-assembled functions (compares and their readers, stack loads and stores, a
  loop, calls, blr, SVCs, patch sites with a `goto`, setjmp/longjmp, tail exits through b, b.cond and br) generated
  through `gen.py` with and without `--locals`, compiled and run against Unicorn. `--no-callee-saved-trust` and
  `--reg-locals N` test the variants.
- `tools/test_lift.py <nso> <base> [count] [seed] --locals` and `tools/test_simd.py --locals`: every instruction as a
  one-instruction function with locals (`gen.single_with_locals`), against Unicorn.

## Measured (2026-11-01, Ryzen 5 5600X, RTX 3060, 1080p, tools/perf_routes.py, six routes)

- **Main thread:** 4-16% less busy per frame on the full CPU and 13-32% less on 4 cores / 4 threads.
- **CPU per frame:** 5-14% less on the full CPU and 3-17% less on 4 cores / 4 threads.
- **fps:** up 3-10% on the views the GPU or the render thread does not cap (field, castle, Hateno). The render thread
  is runtime code and does not change.
- **Code:** the library is 14% smaller (228 vs 264 MB) and compiles 12% faster.
- **Setup:** generation takes 3.5x as long in Python (main 179 s vs 49 s).

## Against PR #324 (flags only in a local, by Hiorac)

Measured 2026-11-01 on the same base and machine, interleaved, six routes at 1080p.

| | #324 | --locals |
|---|---|---|
| main-thread busy, full CPU | -3.5% | -8.2% |
| main-thread busy, 4 cores / 4 threads | -11% | -26% |
| CPU per frame, full CPU | -4.6% | -8.4% |
| CPU per frame, 4 cores / 4 threads | -6% | -10% |
| library | 241.5 MB | 227.6 MB |
| -O1 instructions, first 400 functions of main | -14% | -23% |

- Both flag passes reach about the same 57,000 functions.
- #324's one idea we lacked, that a jump out of a function stores the flags only when the code it goes to can read
  them, is in `gen.py` (`_entry_flags`).
