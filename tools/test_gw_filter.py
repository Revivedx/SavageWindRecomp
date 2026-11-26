#!/usr/bin/env python3
"""Check the block bitmap in front of the storing-shader range search (cpuopts bit 128, exec.c gw_maybe).

The real gw_add, gw_find, gw_overlaps (wrapper and exact search), gw_maybe, gw_blocks_add and gw_ubo_near are extracted
from exec.c and driven with random sets of ranges (small buffers, megabytes, a range over the 4 GB the bitmap hashes,
ranges that touch or overlap, a full set) and random queries (on range ends, one byte either side, on block borders,
wide, empty, wrapped). Required, for every query:
  - the answer with the bit on equals the answer with the bit off (the filter may only say "maybe"),
  - for a non-empty query it equals a plain scan of every range ever added,
  - gw_ubo_near is false only when no constant buffer of the stage overlaps the set.

    tools/test_gw_filter.py [--exec runtime/nvn/vulkan/exec.c]
    tools/test_gw_filter.py --self-check    (breaks the code in six ways; each must be caught)
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def function(source, name):
    match = re.search(r'^static [^;{}]*?\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    brace = match.end() - 1
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


def line(source, prefix):
    i = source.index(prefix)
    return source[i:source.index('\n', i)]


FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t addr, size; } Range;
static struct { Range ubuf[6][18]; } st;
static int opts_value;
static int stage_cpu_opts(void) { return opts_value; }
@DECLS@
@FUNCS@

static void reset(void) {  // (the lines of vk_exec_reset that clear the set)
    ngw = 0;
    gw_gen++;
    gw_lo = ~0ull;
    gw_hi = 0;
    memset(gw_blocks, 0, sizeof gw_blocks);
}

static uint64_t rng_state = 88172645463325252ull;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }

#define HEAP 0x7240000000ull
#define MB (1ull << 20)
#define MAXR 20000
static uint64_t rlo[MAXR], rhi[MAXR];
static int nr;

static uint64_t pick(void) {  // an address: mostly the heap's 4 GB, sometimes elsewhere
    switch (rnd() % 8) {
    case 0: return rnd() % (1ull << 40);
    case 1: return HEAP + (rnd() % 8192) * MB + (rnd() % 4) * (MB - 1);  // block borders
    default: return HEAP + rnd() % (4096 * MB);
    }
}
static uint64_t pick_size(void) {
    uint64_t r = rnd() % 60;
    if (r < 6) return 1 + rnd() % 16;
    if (r < 8) return (1 + rnd() % 40) * MB;
    if (r == 8) return 1 + rnd() % (64 * MB);
    if (r == 9) return rnd() % 12 ? 16 + rnd() % 256 : (4097 + rnd() % 3) * MB;  // (over the bitmap's 4096 blocks)
    return 16 + rnd() % 0x10000;
}

static bool oracle(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < nr; i++) if (rlo[i] < hi && lo < rhi[i]) return true;
    return false;
}

int main(void) {
    long queries = 0, yes = 0, filtered = 0;
    for (int round = 0; round < 400; round++) {
        reset();
        nr = 0;
        int adds = (int)(rnd() % 3 == 0 ? rnd() % 10 : rnd() % 100);
        if (round % 50 == 49) adds = 5000;  // (over GW_MAX: the last ones don't fit)
        for (int a = 0; a < adds; a++) {
            uint64_t lo = pick(), n = pick_size();
            if (rnd() % 5 == 0 && nr) lo = (rnd() & 1 ? rhi : rlo)[rnd() % nr];  // touching or sharing an end
            if (lo + n < lo) continue;
            if (gw_add(lo, lo + n) && nr < MAXR) { rlo[nr] = lo; rhi[nr] = lo + n; nr++; }
        }
        for (int q = 0; q < 4000; q++) {
            uint64_t lo, hi;
            int kind = (int)(rnd() % 8);
            if (kind < 3 && nr) {  // on the ends of a range, one byte each side
                int r = (int)(rnd() % nr);
                uint64_t e = (rnd() & 1) ? rlo[r] : rhi[r];
                lo = e + (rnd() % 5) - 2 - (rnd() % 3 ? 0 : rnd() % (MB + 2));
                hi = lo + 1 + rnd() % (rnd() % 3 ? 64 : 3 * MB);
            } else if (kind == 3) {  // a block border
                lo = ((pick() >> 20) << 20) + (rnd() % 5) - 2;
                hi = lo + 1 + rnd() % 4;
            } else if (kind == 4) {  // wide
                lo = pick();
                hi = lo + (8 + rnd() % 6000) * MB + rnd() % MB;
            } else if (kind == 5) {  // empty or wrapped
                lo = pick();
                hi = lo - (rnd() % 3 ? 0 : rnd() % 100);
            } else {
                lo = pick();
                hi = lo + 16 + rnd() % 0x10000;
            }
            opts_value = 0;
            bool off = gw_overlaps(lo, hi);
            opts_value = 128;
            bool on = gw_overlaps(lo, hi);
            queries++; yes += off;
            if (on != off) { printf("FAIL round %d: [%llx, %llx) bit off %d, bit on %d\n", round, (unsigned long long)lo, (unsigned long long)hi, off, on); return 1; }
            if (hi > lo && off != oracle(lo, hi)) { printf("FAIL round %d: [%llx, %llx) search %d, scan %d\n", round, (unsigned long long)lo, (unsigned long long)hi, off, oracle(lo, hi)); return 1; }
            if (!gw_maybe(lo, hi)) filtered++;
        }
        for (int q = 0; q < 300; q++) {  // gw_ubo_near against the exact answers
            int stage = (int)(rnd() % 6);
            memset(st.ubuf[stage], 0, sizeof st.ubuf[stage]);
            for (int i = 0; i < 18; i++)
                if (rnd() % 3) {
                    uint64_t lo = (rnd() % 4 == 0 && nr) ? rlo[rnd() % nr] - (rnd() % 64) : pick();
                    st.ubuf[stage][i] = (Range){lo, rnd() % 4 ? 16 + rnd() % 0x20000 : 0};
                }
            uint32_t mask = (uint32_t)rnd() & 0x1fffff8;
            bool any = false;
            for (uint32_t m = mask >> 3; m; m &= m - 1) {
                int i = __builtin_ctz(m);
                if (i >= 18) break;
                Range r = st.ubuf[stage][i];
                uint64_t size = r.size ? (r.size > 0x10000 ? 0x10000 : r.size) : 0x10000;
                opts_value = 0;
                if (r.addr && gw_overlaps(r.addr, r.addr + size)) any = true;
            }
            if (any && !gw_ubo_near(stage, mask)) { printf("FAIL round %d: gw_ubo_near false with a constant buffer in the set\n", round); return 1; }
        }
    }
    printf("ok: %ld queries (%ld inside the set), same answers with the bit on and off, equal to a scan, %ld ended on the bitmap\n", queries, yes, filtered);
    if (yes < queries / 50 || filtered < queries / 4) { printf("FAIL: the queries do not exercise both answers\n"); return 1; }
    return 0;
}
'''


def build(src, mutate=None):
    reset = src[src.index('void vk_exec_reset(void) {'):]
    assert 'memset(gw_blocks, 0, sizeof gw_blocks);' in reset[:reset.index('\n}\n')], 'vk_exec_reset no longer clears the bitmap'
    decls = ['#define GW_MAX 4096',
             line(src, 'static struct { uint64_t lo, hi; } gw[GW_MAX];'),
             line(src, 'static int ngw;'),
             line(src, 'static uint64_t gw_lo = ~0ull, gw_hi;'),
             line(src, 'static uint32_t gw_gen = 1;'),
             line(src, 'static bool gw_overlaps_exact(uint64_t lo, uint64_t hi);'),
             line(src, '#define GW_BLOCK_SHIFT'),
             line(src, '#define GW_BLOCK_BITS'),
             line(src, 'static uint64_t gw_blocks[')]
    funcs = [function(src, n) for n in ('gw_find', 'gw_maybe', 'gw_blocks_add', 'gw_overlaps', 'gw_overlaps_exact', 'gw_add', 'gw_ubo_near')]
    text = FIXTURE.replace('@DECLS@', '\n'.join(decls)).replace('@FUNCS@', '\n'.join(funcs))
    if mutate:
        old, new = mutate
        assert old in text, old
        text = text.replace(old, new, 1)
    return text


def run(text, tmp):
    c = Path(tmp) / 't.c'
    c.write_text(text)
    exe = Path(tmp) / 't'
    r = subprocess.run(['clang', '-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-w', str(c), '-o', str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
    r = subprocess.run([str(exe)], capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
    return r.returncode, r.stdout + r.stderr


MUTANTS = [
    ('query block range one short', 'b1 = (hi - 1) >> GW_BLOCK_SHIFT;\n    if (b1 - b0 >= 8)', 'b1 = (hi - 2) >> GW_BLOCK_SHIFT;\n    if (b1 - b0 >= 8)'),
    ('added block range one short', 'b1 = hi > lo ? (hi - 1) >> GW_BLOCK_SHIFT : b0;', 'b1 = hi > lo ? (hi - 2) >> GW_BLOCK_SHIFT : b0;'),
    ('wide query says no', 'if (b1 - b0 >= 8) return true;', 'if (b1 - b0 >= 8) return false;'),
    ('only the first block is added', 'gw_blocks_add(lo, hi);\n    return true;', 'gw_blocks_add(lo, lo + 1);\n    return true;'),
    ('empty query says no', 'if (hi <= lo) return true;', 'if (hi <= lo) return false;'),
    ('bit index differs when asked', 'if (gw_blocks[(b & (GW_BLOCK_BITS - 1)) >> 6] >> (b & 63) & 1) return true;', 'if (gw_blocks[(b & (GW_BLOCK_BITS - 1)) >> 6] >> ((b + 1) & 63) & 1) return true;'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exec', default=str(Path(__file__).resolve().parent.parent / 'runtime/nvn/vulkan/exec.c'))
    ap.add_argument('--self-check', action='store_true')
    a = ap.parse_args()
    src = Path(a.exec).read_text()
    with tempfile.TemporaryDirectory() as tmp:
        code, out = run(build(src), tmp)
        print(out.strip().splitlines()[-1] if out.strip() else out)
        if code:
            print(out)
            return 1
        if a.self_check:
            bad = 0
            for name, old, new in MUTANTS:
                code, out = run(build(src, (old, new)), tmp)
                print('%-32s %s' % (name, 'caught' if code else 'MISSED'))
                bad += not code
            return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
