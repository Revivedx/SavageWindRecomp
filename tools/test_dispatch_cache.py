#!/usr/bin/env python3
"""Check the per-thread dispatch table (cpuopts bit 1024, runtime/dispatch.c dispatch_fast) against the original.

The real runtime/dispatch.c is compiled into a harness with stand-ins for the scheduler and the game code (a few hundred
generated guest functions, implemented and unimplemented host functions, overrides). A long random sequence of calls
goes through rt_dispatch on several threads, with the flag switched live, the unwind flag raised and lowered, and the
scheduler's preemption condition true now and then. After every call the harness compares with an oracle:

  - which function ran (an ordinary one, an override, a host function, "unimplemented" with the right slot),
  - the events before it and their order: a preemption when the thread's core has a better waiter, the reboot's unwind
    point at every host call while the unwind flag is up (the cached path must not skip it),
  - at the end of each phase, every entry of every thread's table: an even address holds the function at that address,
    an odd one (the host tag) holds an implemented host function of the right slot, never an unimplemented one.

Further: a call from a Cpu without a thread, an override at an odd address (it must not be cached where it would answer
for the even address below it), and four host threads calling at once (their tables are their own).

    tools/test_dispatch_cache.py [--source runtime/dispatch.c]
    tools/test_dispatch_cache.py --self-check    (breaks the code in eight ways; each must be caught)
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

NGUEST = 300
NHLE = 24          # implemented host functions
NUNIMPL = 6        # host stubs without an implementation
NOVER = 4          # overrides (the last one at an odd address)


def harness(dispatch_c):
    gen = []
    for i in range(NGUEST):
        gen.append(f'static void gf{i}(Cpu *c) {{ (void)c; tl_last = (void *)gf{i}; }}')
    for i in range(NHLE):
        gen.append(f'static void hf{i}(Cpu *c) {{ (void)c; tl_last = (void *)hf{i}; }}')
    for i in range(NOVER):
        gen.append(f'static void of{i}(Cpu *c) {{ (void)c; tl_last = (void *)of{i}; }}')
    gen.append('static const GuestFn gfs[NGUEST] = {' + ','.join(f'gf{i}' for i in range(NGUEST)) + '};')
    gen.append('static const HostFn hfs[NHLE] = {' + ','.join(f'hf{i}' for i in range(NHLE)) + '};')
    gen.append('static const HostFn ofs[NOVER] = {' + ','.join(f'of{i}' for i in range(NOVER)) + '};')
    return FIXTURE.replace('@GEN@', '\n'.join(gen)).replace('@DISPATCH@', str(dispatch_c)) \
        .replace('@NGUEST@', str(NGUEST)).replace('@NHLE@', str(NHLE)).replace('@NUNIMPL@', str(NUNIMPL)) \
        .replace('@NOVER@', str(NOVER))


FIXTURE = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "@DISPATCH@"

__attribute__((section("hle_impls"), used)) static const HleImpl hle_section_anchor = {0, 0};  // (the linker defines the section's bounds)

#define NGUEST @NGUEST@
#define NHLE @NHLE@
#define NUNIMPL @NUNIMPL@
#define NOVER @NOVER@

// ---- stand-ins for the rest of the runtime ----
CoreBestWait g_core_best_wait[4];
volatile int g_rt_unwind;
static __thread char ev[16];
static __thread int nev;
static __thread void *tl_last;
static void evlog(char ch) { if (nev < 15) { ev[nev++] = ch; ev[nev] = 0; } }
void rt_unwind_point(void) { evlog('U'); }
void kernel_preempt(KThread *t) { evlog('P'); g_core_best_wait[t->core_held].v = 1000; }
static uint32_t nmod = 1;
static uint64_t gcount = NGUEST;
static uint64_t gaddr[NGUEST];
static GameLibModule gmod;
static GameLibTables gtab;
const GameLibTables *gamelib_tables(void) { return &gtab; }

@GEN@

static uint64_t rng_state;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

// ---- the world the oracle knows ----
static uint64_t hle_pc[NHLE + NUNIMPL];  // implemented first, then the unimplemented
static uint64_t over_pc[NOVER];
static uint64_t odd_over_pc, odd_below_pc;
#define POOL (NGUEST + NHLE + NUNIMPL + NOVER)
static uint64_t pool[POOL + 2];
static int npool;

static void *expected(uint64_t pc, int *is_hle, int *unimpl_slot) {
    *is_hle = 0; *unimpl_slot = -1;
    if (pc >= HLE_BASE && pc < HLE_BASE + HLE_SIZE) {
        int i = (int)((pc - HLE_BASE) / 16);
        *is_hle = 1;
        if (i < NHLE) return (void *)hfs[i];
        *unimpl_slot = i;
        return NULL;
    }
    for (int i = 0; i < NOVER; i++) if (over_pc[i] == pc) return (void *)ofs[i];
    for (int i = 0; i < NGUEST; i++) if (gaddr[i] == pc) return (void *)gfs[i];
    abort();
}

typedef struct { KThread *t; Cpu *c; } Rig;
static long checked, pre_seen, unwind_seen;

// One call through rt_dispatch with the oracle. fast: the flag as it stands now.
static void call_checked(Rig *r, uint64_t pc, int fast, int unwinding) {
    Cpu *c = r->c;
    c->pc = pc;
    int is_hle, slot;
    void *want = expected(pc, &is_hle, &slot);
    KThread *t = c->thread;
    int want_p = t && t->core_held >= 0 && g_core_best_wait[t->core_held].v < t->prio;
    int before = slot >= 0 ? unimpl_counts[slot] : 0;
    tl_last = (void *)1; nev = 0; ev[0] = 0;
    rt_dispatch(c);
    checked++;
    if (want_p) pre_seen++;
    if (slot >= 0) {
        if (tl_last != (void *)1 || unimpl_counts[slot] != before + 1 || unimpl_pc != pc) {
            fprintf(stderr, "pc %llx: the unimplemented stub of slot %d did not run (last %p, count %d -> %d, unimpl_pc %llx)\n",
                    (unsigned long long)pc, slot, tl_last, before, unimpl_counts[slot], (unsigned long long)unimpl_pc);
            exit(1);
        }
    } else if (tl_last != want) {
        fprintf(stderr, "pc %llx: ran %p, expected %p (fast=%d)\n", (unsigned long long)pc, tl_last, want, fast);
        exit(1);
    }
    // events: P (preemption) first, then U (the unwind point) at a host call
    char exp_a[4], exp_b[4];
    snprintf(exp_a, sizeof exp_a, "%s%s", want_p ? "P" : "", is_hle ? "U" : "");  // every call passes it (the old body)
    snprintf(exp_b, sizeof exp_b, "%s", want_p ? "P" : "");                       // a hit of the new body needs not
    int ok;
    if (!is_hle || !t || !fast) ok = !strcmp(ev, exp_a);
    else if (unwinding) ok = !strcmp(ev, exp_a);                                  // while rebooting it must
    else ok = !strcmp(ev, exp_a) || !strcmp(ev, exp_b);
    if (!ok) {
        fprintf(stderr, "pc %llx: events \"%s\", expected \"%s\" (hle=%d fast=%d unwinding=%d thread=%d)\n",
                (unsigned long long)pc, ev, exp_a, is_hle, fast, unwinding, t != NULL);
        exit(1);
    }
    if (is_hle && unwinding) unwind_seen++;
}

static void check_table(KThread *t) {
    DEntry *tab = (DEntry *)t->dcache;
    if (!tab) return;
    for (int i = 0; i < DCACHE; i++) {
        uint64_t a = tab[i].addr;
        if (!a) continue;
        uint64_t pc = a & ~1ull;
        if ((int)((pc >> 2) & (DCACHE - 1)) != i) { fprintf(stderr, "entry %d holds %llx (wrong slot)\n", i, (unsigned long long)a); exit(1); }
        int is_hle, slot;
        void *want = expected(pc, &is_hle, &slot);
        if (slot >= 0) { fprintf(stderr, "entry %d caches the unimplemented stub %llx\n", i, (unsigned long long)a); exit(1); }
        if ((a & 1) != (uint64_t)is_hle) { fprintf(stderr, "entry %d: %llx is tagged wrongly (hle=%d)\n", i, (unsigned long long)a, is_hle); exit(1); }
        if ((void *)tab[i].fn != want) { fprintf(stderr, "entry %d: %llx holds %p, expected %p\n", i, (unsigned long long)a, (void *)tab[i].fn, want); exit(1); }
    }
}

static Rig make_rig(int with_thread, int prio, int core) {
    Rig r;
    r.t = calloc(1, sizeof(KThread));
    r.c = &r.t->cpu;
    r.t->prio = prio;
    r.t->core_held = core;
    r.c->thread = with_thread ? r.t : NULL;
    return r;
}

static void shuffle_core_waiters(void) {
    static const int vals[3] = {1000, 10, 40};
    for (int i = 0; i < 4; i++) g_core_best_wait[i].v = vals[rnd() % 3];
}

#define NTHREADS 4
typedef struct { Rig r; uint64_t seed; long calls; int bad; } Worker;

static void *worker_main(void *arg) {
    Worker *w = arg;
    rng_state = w->seed;
    for (long i = 0; i < w->calls; i++) {
        uint64_t pc = pool[rnd() % npool];
        w->r.c->pc = pc;
        int is_hle, slot;
        void *want = expected(pc, &is_hle, &slot);
        tl_last = (void *)1;
        rt_dispatch(w->r.c);
        if (slot >= 0 ? tl_last != (void *)1 : tl_last != want) { w->bad = 1; return NULL; }
    }
    return NULL;
}

int main(int argc, char **argv) {
    long calls = argc > 1 ? atol(argv[1]) : 400000;
    rng_state = 0x9e3779b97f4a7c15ull;
    // guest functions: sorted addresses, some sharing a slot of the 4096-entry table (0x4000 apart), some neighbours
    uint64_t a = 0x7100000;
    for (int i = 0; i < NGUEST; i++) {
        a += (rnd() % 3 == 0) ? 0x4000 : 4 * (1 + rnd() % 40);
        gaddr[i] = a;
    }
    // (an address 0x4000 above another shares its slot; the sequence above is strictly increasing)
    static uint64_t lo_addr[1];
    lo_addr[0] = gaddr[0];
    gmod.addrs = gaddr; gmod.fns = gfs; gmod.count = &gcount;
    gtab.modules = &gmod; gtab.nmodules = &nmod;
    dispatch_init();
    for (int i = 0; i < NHLE; i++) {
        char name[32]; snprintf(name, sizeof name, "impl%d", i);
        hle_pc[i] = hle_register(name, hfs[i]);
    }
    for (int i = 0; i < NUNIMPL; i++) {
        char name[32]; snprintf(name, sizeof name, "stub%d", i);
        hle_pc[NHLE + i] = hle_register(name, NULL);
    }
    // overrides: three on guest-style addresses (above the function table), the last at an odd address just above one
    // of the guest functions (the even address below it is the function)
    for (int i = 0; i < NOVER - 1; i++) {
        over_pc[i] = 0x7300000 + 0x40 * i;
        hle_override(over_pc[i], ofs[i], "o");
    }
    odd_below_pc = gaddr[NGUEST / 2];
    odd_over_pc = odd_below_pc + 1;
    over_pc[NOVER - 1] = odd_over_pc;
    hle_override(odd_over_pc, ofs[NOVER - 1], "odd");
    for (int i = 0; i < NGUEST; i++) pool[npool++] = gaddr[i];
    for (int i = 0; i < NHLE + NUNIMPL; i++) pool[npool++] = hle_pc[i];
    for (int i = 0; i < NOVER; i++) pool[npool++] = over_pc[i];

    Rig rigs[4] = {make_rig(1, 20, 0), make_rig(1, 30, 1), make_rig(1, 45, -1), make_rig(0, 0, -1)};

    // phase 1: the flag is off. Nothing may touch the threads' tables.
    g_opt_dispatch_fast = 0;
    g_rt_unwind = 0;
    for (long i = 0; i < calls / 4; i++) {
        Rig *r = &rigs[rnd() % 4];
        if (i % 7 == 0) shuffle_core_waiters();
        call_checked(r, pool[rnd() % npool], 0, 0);
        if (r->t->dcache) { fprintf(stderr, "the old body allocated a thread table\n"); return 1; }
    }
    // phase 2: the flag is on, switched off and on again at random, the unwind flag up in stretches
    int fast = 1, unwinding = 0;
    for (long i = 0; i < calls; i++) {
        if (i % 97 == 0) { fast = (rnd() % 5) != 0; g_opt_dispatch_fast = fast; }
        if (i % 61 == 0) { unwinding = (rnd() % 3) == 0; g_rt_unwind = unwinding; }
        if (i % 5 == 0) shuffle_core_waiters();
        Rig *r = &rigs[rnd() % 4];
        // the odd override and the function below it, back to back (a cached odd address would answer for both)
        if (i % 211 == 0) { call_checked(r, odd_over_pc, fast, unwinding); call_checked(r, odd_below_pc, fast, unwinding); }
        call_checked(r, pool[rnd() % npool], fast, unwinding);
        if (i % 20011 == 0) for (int k = 0; k < 3; k++) check_table(rigs[k].t);
    }
    for (int k = 0; k < 3; k++) check_table(rigs[k].t);
    if (!rigs[3].t->dcache == 0) { fprintf(stderr, "a Cpu without a thread got a table\n"); return 1; }
    int used = 0;
    for (int k = 0; k < 3; k++) used += rigs[k].t->dcache != NULL;
    if (used != 3) { fprintf(stderr, "only %d of 3 threads used a table with the flag on\n", used); return 1; }
    if (!pre_seen || !unwind_seen) { fprintf(stderr, "the run never preempted (%ld) or unwound at a host call (%ld)\n", pre_seen, unwind_seen); return 1; }

    // phase 3: four host threads call at once with the flag on (and off in between); each has its own table
    g_rt_unwind = 0;
    for (int i = 0; i < 4; i++) g_core_best_wait[i].v = 1000;
    pthread_t th[NTHREADS];
    Worker ws[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        ws[i].r = make_rig(1, 20 + i, -1);
        ws[i].seed = 0x1234567ull * (i + 1) + 1;
        ws[i].calls = calls / 2;
        ws[i].bad = 0;
    }
    g_opt_dispatch_fast = 1;
    for (int i = 0; i < NTHREADS; i++) pthread_create(&th[i], NULL, worker_main, &ws[i]);
    for (int k = 0; k < 40; k++) { g_opt_dispatch_fast = k & 1 ? 1 : 0; usleep(300); }
    g_opt_dispatch_fast = 1;
    for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < NTHREADS; i++) {
        if (ws[i].bad) { fprintf(stderr, "worker %d ran a wrong function\n", i); return 1; }
        check_table(ws[i].r.t);
    }
    printf("dispatch cache: %ld checked calls (%ld with a preemption, %ld host calls while unwinding), 3 tables and 4 racing threads verified\n",
           checked, pre_seen, unwind_seen);
    return 0;
}
'''

UNREACHED = ['control_mem_peek', 'fs_recent_files', 'kernel_current_cpu', 'kernel_current_thread_handle',
             'kernel_log_guest_chain', 'loader_find_symbol', 'loader_symbol_at', 'plat_commit',
             'plat_crash_handler_install', 'plat_fatal_notice', 'plat_now_ns', 'plat_print_backtrace', 'plat_protect',
             'plat_symbol']

ENVS = [
    ('-O2', []),
    ('-O1', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']),
]

MUTANTS = [
    ('the hit of a host entry ignores the unwind flag', ' && !g_rt_unwind) __attribute__', ') __attribute__'),
    ('an unimplemented stub is cached', 'kind <= 1 && !(pc & 1)', 'kind <= 2 && !(pc & 1)'),
    ('host entries are not tagged', 'e->addr = kind ? pc | 1 : pc;', 'e->addr = pc;'),
    ('an odd address is cached', 'kind <= 1 && !(pc & 1)', 'kind <= 1'),
    ('the hit path never looks at the preemption', 'if (__builtin_expect(t->core_held >= 0 && g_core_best_wait[t->core_held].v < t->prio, 0))\n        __attribute__((musttail)) return dispatch_fast_slow(c);', ''),
    ('the slow path skips the preemption', 'if (t->core_held >= 0 && g_core_best_wait[t->core_held].v < t->prio) kernel_preempt(t);\n    uint64_t pc = c->pc;\n    DEntry *tab', 'uint64_t pc = c->pc;\n    DEntry *tab'),
    ('the slow path skips the unwind point of a cached host entry', 'if (e && e->addr == (pc | 1)) {  // (an HLE target)\n        rt_unwind_point();', 'if (e && e->addr == (pc | 1)) {  // (an HLE target)\n        '),
    ('a Cpu without a thread is not sent to the old body', 'if (__builtin_expect(!t, 0)) __attribute__((musttail)) return dispatch_old(c);', ''),
]


def build(source_text, workdir, tag, opt, extra):
    src = Path(workdir) / f'dispatch_{tag}.c'
    src.write_text(source_text)
    h = Path(workdir) / f'harness_{tag}.c'
    h.write_text(harness(src))
    exe = Path(workdir) / f'harness_{tag}'
    stubs = Path(workdir) / 'stubs.c'  # (the runtime's other functions dispatch.c names but this test never reaches)
    stubs.write_text('#include <stdlib.h>\n' + ''.join(f'void {n}(void) {{ abort(); }}\n' for n in UNREACHED))
    cmd = ['clang', opt, '-g', '-pthread', '-I', str(ROOT / 'runtime'), '-I', str(ROOT), '-w',
           *extra, str(h), str(stubs), '-o', str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'compile failed ({tag}):\n{r.stderr[-3000:]}')
    return exe


def run(exe, calls):
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    r = subprocess.run([str(exe), str(calls)], capture_output=True, text=True, env=env, timeout=600)
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', default=str(ROOT / 'runtime' / 'dispatch.c'))
    ap.add_argument('--calls', type=int, default=400000)
    ap.add_argument('--self-check', action='store_true')
    args = ap.parse_args()
    original = Path(args.source).read_text()
    with tempfile.TemporaryDirectory() as work:
        if not args.self_check:
            for i, (opt, extra) in enumerate(ENVS):
                code, out = run(build(original, work, f'e{i}', opt, extra), args.calls)
                done = [l for l in out.splitlines() if l.startswith('dispatch cache:')]
                print(done[-1] if done else '(no result line)', f'[{opt} {" ".join(extra)}]')
                if code:
                    print(out)
                    return 1
            return 0
        missed = []
        for n, (what, old, new) in enumerate(MUTANTS):
            if original.count(old) != 1:
                print(f'mutant {n} ({what}): the pattern is not in the source exactly once ({original.count(old)})')
                return 2
            exe = build(original.replace(old, new), work, f'm{n}', '-O2', [])
            code, out = run(exe, args.calls)
            out = '\n'.join(l for l in out.splitlines() if not l.startswith('[hle] UNIMPLEMENTED'))
            caught = code != 0
            print(f'mutant {n}: {what}: {"caught" if caught else "NOT CAUGHT"}' + (f' ({out.splitlines()[0][:90]})' if caught and out else ''), flush=True)
            if not caught:
                missed.append(what)
        if missed:
            print('not caught:', missed)
            return 1
        print(f'all {len(MUTANTS)} mutants caught')
        return 0


if __name__ == '__main__':
    sys.exit(main())
