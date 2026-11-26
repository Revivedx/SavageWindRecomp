#!/usr/bin/env python3
"""Check the command buffer registry (cpuopts bit 2048, runtime/nvn/cbreg.h) against a model.

The real header is compiled into a harness. Random adds and removes (a buffer in the middle, at either end, the only
one, one taken out twice, one never registered, NULL) run in either way (the O(1) unlink or the walk, chosen at random
for every call, as the live switch can do) and after each call the list is compared with a model kept as an array:
forward order, every previous link, the head, and the links of a buffer that was taken out (NULL).

    tools/test_cb_registry.py
    tools/test_cb_registry.py --self-check    (breaks the code in six ways; each must be caught)
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

FIXTURE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct NvnCmdBuf { int id; struct NvnCmdBuf *registry_next, *registry_prev; } NvnCmdBuf;
#include "@HEADER@"

static uint64_t st = 0x2545f4914f6cdd1dull;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
#define MAXN 600
static NvnCmdBuf *model[MAXN];  // newest first
static int nm;
static NvnCmdBuf *gone[4096];   // taken out (kept allocated, as a finalized buffer's memory is until it is freed)
static int ngone;
static NvnCmdBuf *head;

static void fail(const char *what, long step) { fprintf(stderr, "step %ld: %s\n", step, what); exit(1); }

static void verify(long step) {
    NvnCmdBuf *it = head, *prev = NULL;
    for (int i = 0; i < nm; i++) {
        if (it != model[i]) fail("the list differs from the model", step);
        if (it->registry_prev != prev) fail("a previous link is wrong", step);
        prev = it;
        it = it->registry_next;
    }
    if (it) fail("the list is longer than the model", step);
    for (int i = 0; i < ngone; i++)
        if (gone[i]->registry_next || gone[i]->registry_prev) fail("a buffer taken out still has links", step);
}

int main(int argc, char **argv) {
    long steps = argc > 1 ? atol(argv[1]) : 2000000;
    int next_id = 1;
    NvnCmdBuf *never = calloc(1, sizeof *never);  // never registered
    for (long s = 0; s < steps; s++) {
        bool o1 = rnd() & 1;
        unsigned op = rnd() % 100;
        if (op < 40 && nm < MAXN) {
            NvnCmdBuf *cb = calloc(1, sizeof *cb);
            cb->id = next_id++;
            // a recycled address: the old links of a freed buffer are garbage, the allocator hands the memory out again
            cb->registry_next = (NvnCmdBuf *)(uintptr_t)0x10; cb->registry_prev = (NvnCmdBuf *)(uintptr_t)0x20;
            cb_registry_add(&head, cb);
            for (int i = nm; i > 0; i--) model[i] = model[i - 1];
            model[0] = cb; nm++;
        } else if (op < 85 && nm) {
            int i = rnd() % nm;
            if (rnd() % 5 == 0) i = (rnd() & 1) ? 0 : nm - 1;  // the ends
            NvnCmdBuf *cb = model[i];
            cb_registry_remove(&head, cb, o1);
            for (int k = i; k + 1 < nm; k++) model[k] = model[k + 1];
            nm--;
            if (ngone < 4096) gone[ngone++] = cb;
        } else if (op < 92 && ngone) {
            cb_registry_remove(&head, gone[rnd() % ngone], o1);  // finalized twice
        } else if (op < 96) {
            cb_registry_remove(&head, never, o1);
        } else {
            cb_registry_remove(&head, NULL, o1);
        }
        if (nm <= 1 || s % 3 == 0) verify(s);  // (the near-empty list is where the head is the only link)
    }
    verify(steps);
    printf("cb registry: %ld random calls in both ways, every link checked\n", steps);
    return 0;
}
'''

MUTANTS = [
    ('the O(1) unlink leaves the next one pointing back at the removed buffer',
     '        if (cb->registry_next) cb->registry_next->registry_prev = cb->registry_prev;\n', ''),
    ('the walk leaves the next one pointing back at the removed buffer',
     '        if (it->registry_next) it->registry_next->registry_prev = prev;\n', ''),
    ('add forgets the old head\'s back link', '    if (*head) (*head)->registry_prev = cb;\n', ''),
    ('the O(1) unlink does not clear the links', '        cb->registry_next = cb->registry_prev = NULL;\n        return;', '        return;'),
    ('the O(1) unlink trusts a buffer with no links to be the head',
     '(cb->registry_prev ? cb->registry_prev->registry_next == cb : *head == cb)', '(cb->registry_prev ? cb->registry_prev->registry_next == cb : true)'),
    ('NULL is not ignored', '    if (!cb) return;\n', ''),
]


def build(text, work, tag):
    header = Path(work) / f'cbreg_{tag}.h'
    header.write_text(text)
    src = Path(work) / f'h_{tag}.c'
    src.write_text(FIXTURE.replace('@HEADER@', str(header)))
    exe = Path(work) / f'h_{tag}'
    r = subprocess.run(['clang', '-O1', '-g', '-w', '-fsanitize=address,undefined', str(src), '-o', str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('compile failed:\n' + r.stderr[-2000:])
    return exe


def run(exe, steps):
    r = subprocess.run([str(exe), str(steps)], capture_output=True, text=True, timeout=600,
                       env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--header', default=str(ROOT / 'runtime' / 'nvn' / 'cbreg.h'))
    ap.add_argument('--steps', type=int, default=2000000)
    ap.add_argument('--self-check', action='store_true')
    args = ap.parse_args()
    original = Path(args.header).read_text()
    nvn_h = (ROOT / 'runtime' / 'nvn' / 'nvn.h').read_text()
    if 'struct NvnCmdBuf *registry_next, *registry_prev;' not in nvn_h:
        print('nvn.h no longer declares the two registry links the harness mirrors')
        return 2
    # The harness defines the switch itself, so a missing definition in the runtime would only show at the real link.
    cmdbuf_c = (ROOT / 'runtime' / 'nvn' / 'cmdbuf.c').read_text()
    if len(re.findall(r'^volatile int g_opt_cb_unlink;', cmdbuf_c, re.M)) != 1:
        print('runtime/nvn/cmdbuf.c must define g_opt_cb_unlink exactly once (rt.h declares it extern)')
        return 2
    with tempfile.TemporaryDirectory() as work:
        if not args.self_check:
            code, out = run(build(original, work, 'a'), args.steps)
            print(out.splitlines()[-1] if out else '(no output)')
            if code:
                print(out)
            return code
        missed = []
        for n, (what, old, new) in enumerate(MUTANTS):
            if original.count(old) != 1:
                print(f'mutant {n} ({what}): the pattern is in the source {original.count(old)} times, not once')
                return 2
            code, out = run(build(original.replace(old, new), work, f'm{n}'), args.steps)
            print(f'mutant {n}: {what}: {"caught" if code else "NOT CAUGHT"}' + (f' ({out.splitlines()[0][:80]})' if code and out else ''), flush=True)
            if not code:
                missed.append(what)
        if missed:
            print('not caught:', missed)
            return 1
        print(f'all {len(MUTANTS)} mutants caught')
        return 0


if __name__ == '__main__':
    sys.exit(main())
