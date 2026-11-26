#!/usr/bin/env python3
"""Check the cache-line guest copy table (cpuopts bit 64, exec.c guest_copy_line) against the original table.

The real functions are extracted from exec.c and compiled with stand-ins for the transient allocator. A model of guest
memory is written to the way the game and UpdateUniformBuffer write it, and every copy a call hands back is compared
with the memory as it is at that moment (the oracle). The original table and the line table must also agree on every
call, hit or miss, because they have the same slot function, ways and replacement.

    tools/test_guest_copy_line.py [--exec runtime/nvn/vulkan/exec.c]
    tools/test_guest_copy_line.py --self-check    (breaks the code in five ways; each must be caught)
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def function(source, name):
    start = source.index('static ', source.rfind('\n', 0, source.index(name + '(')) + 1)
    brace = source.index('{', source.index(name + '(', start))
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


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
typedef struct { const void *buf; uint64_t off; void *ptr; } VkSpan;
typedef struct { uint64_t seq; } Chunk;
static Chunk chunks[8];
static int active;
static uint64_t pending = 1;
static uint64_t vk_pending_seq(void) { return pending; }
static uint64_t stat_gcopy_hit, stat_gcopy_miss, stat_gcopy_bytes;
static int opts_value;
static int stage_cpu_opts(void) { return opts_value; }
#define MEM (1u << 20)
static unsigned char *mem;
static VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    (void)align;
    unsigned char *p = malloc(size);  // (never freed, leak check is off; ASan still sees an overrun of a span)
    memcpy(p, data, size);
    return (VkSpan){0, 0, p};
}
#define GCOPY_SETS 2048
#define GCOPY_WAYS 4
#define UUB_MAX 64
static void mvec_sync(void) {}
@DECLS@
@FUNCS@

static uint64_t rng_state;
static uint32_t rnd(void) { rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(rng_state >> 33); }

#define NOPS 300000
static unsigned char hist[2][NOPS];
static int nreq[2];

static void reset(void) {
    memset(gcopy, 0, sizeof gcopy);
    memset(gcopy_rr, 0, sizeof gcopy_rr);
    memset(gcopy_line, 0, sizeof gcopy_line);
    memset(gcopy_line_rr, 0, sizeof gcopy_line_rr);
    memset(uubt, 0, sizeof uubt);
    nuub = 0; uub_serial = 0; gcopy_gen = 1; pending = 1; active = 0;
    memset(chunks, 0, sizeof chunks);
    stat_gcopy_hit = stat_gcopy_miss = stat_gcopy_bytes = 0;
}

static uint64_t twin[3000];  // per hot address: another size that lands in the same table slot (0 if none found)

int main(void) {
    mem = malloc(MEM);
    for (int i = 0; i < 40; i++) {
        uint64_t addr = 256ull * i, base = 16 + 16ull * (i % 12);
        for (uint64_t sz = 16; sz < (1u << 22) && !twin[i]; sz += 16)
            if (sz != base && gcopy_slot(addr, sz) == gcopy_slot(addr, base) && addr + sz <= MEM) twin[i] = sz;
    }
    for (int mode = 0; mode < 2; mode++) {
        opts_value = mode ? 64 : 0;
        reset();
        rng_state = 12345;
        for (unsigned i = 0; i < MEM; i++) mem[i] = (unsigned char)rnd();
        for (int op = 0; op < NOPS; op++) {
            uint32_t k = rnd() % 100;
            if (k < 80) {  // a draw copies a range: a few thousand distinct ones, so the table collides
                bool hot = rnd() % 4 != 0;  // most draws reuse one of forty ranges (same address and size), the rest are new
                uint64_t addr = 256ull * (hot ? rnd() % 40 : rnd() % 3000);
                uint64_t size = hot ? 16 + 16ull * (addr / 256 % 12) : 16 + 16ull * (rnd() % 8) * (1 + rnd() % 3);
                if (hot && !(rnd() % 6) && twin[addr / 256]) size = twin[addr / 256];  // same address, other size, same slot
                if (!(rnd() % 400)) size = 0x10000;
                if (addr + size > MEM) size = MEM - addr;
                uint64_t before = stat_gcopy_hit;
                VkSpan s = guest_copy((uint64_t)(uintptr_t)mem + addr, size);
                if (memcmp(s.ptr, mem + addr, size)) { printf("FAIL mode %d op %d: a copy differs from memory (%llx+%llu)\n", mode, op, (unsigned long long)addr, (unsigned long long)size); return 1; }
                if (nreq[mode] < NOPS) hist[mode][nreq[mode]++] = (unsigned char)(stat_gcopy_hit != before);
            } else if (k < 86) {  // UpdateUniformBuffer: written inline, then noted
                uint64_t lo = 256ull * (rnd() % (rnd() % 2 ? 40 : 3000)) + 16ull * (rnd() % 8);
                uint64_t hi = lo + 16 + 16ull * (rnd() % 24);
                if (hi > MEM) hi = MEM;
                for (uint64_t a = lo; a < hi; a++) mem[a] = (unsigned char)rnd();
                uub_note((uint64_t)(uintptr_t)mem + lo, (uint64_t)(uintptr_t)mem + hi);
            } else if (k < 88) {  // a new list: the game wrote what it likes since
                for (int j = 0; j < 40; j++) { uint64_t a = rnd() % MEM; mem[a] = (unsigned char)rnd(); }
                if (!(rnd() % 8)) for (uint64_t a = 0; a < 3000 * 256; a += 64) mem[a] ^= 1;
                guest_copy_flush();
            } else if (k < 94) {
                pending++;
            } else {
                active = (int)(rnd() % 8);
            }
        }
    }
    if (nreq[0] != nreq[1]) { printf("FAIL: request counts differ\n"); return 1; }
    int hits = 0;
    for (int i = 0; i < nreq[0]; i++) {
        hits += hist[0][i];
        if (hist[0][i] != hist[1][i]) { printf("FAIL: call %d: original %s, line table %s\n", i, hist[0][i] ? "hit" : "miss", hist[1][i] ? "hit" : "miss"); return 1; }
    }
    printf("ok: %d copies, %d hits, same answers in both tables, every copy equal to memory\n", nreq[0], hits);
    return hits > nreq[0] / 20 && hits < nreq[0] * 19 / 20 ? 0 : (printf("FAIL: test fixture does not exercise both hits and misses\n"), 1);
}
'''


def build(src, mutate=None):
    funcs = [function(src, n) for n in ('guest_copy_flush', 'uub_note', 'gcopy_slot', 'guest_copy_line', 'guest_copy')]
    decls = [line(src, 'typedef struct { uint64_t addr, size, gen, serial, mask; int chunk; VkSpan span; } GcopyEntry;'),
             line(src, 'static GcopyEntry gcopy['),
             line(src, 'static uint8_t gcopy_rr['),
             line(src, 'static uint64_t gcopy_gen'),
             line(src, 'static struct { uint64_t lo, hi, serial; } uubt['),
             line(src, 'static int nuub;'),
             line(src, 'static uint64_t uub_serial;'),
             line(src, 'typedef struct { uint64_t addr, gen, serial, mask; uint32_t size, chunk; VkSpan span; } GCopyLine;'),
             line(src, '_Static_assert(sizeof(GCopyLine)'),
             line(src, 'static GCopyLine gcopy_line['),
             line(src, 'static uint8_t gcopy_line_rr[')]
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
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    r = subprocess.run([str(exe)], capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


MUTANTS = [
    ('key address not compared', 'if (e->addr == addr && e->size == (uint32_t)size)', 'if (e->size == (uint32_t)size)'),
    ('key size not compared', 'if (e->addr == addr && e->size == (uint32_t)size)', 'if (e->addr == addr)'),
    ('generation not compared', 'if (e->gen != gen) {', 'if (false && e->gen != gen) {'),
    ('staleness ignored', 'stale = uubt[__builtin_ctzll(m)].serial > e->serial', 'stale = false'),
    ('written ranges not recorded', 'e->mask = mask; e->chunk = (uint32_t)active;', 'e->mask = 0; e->chunk = (uint32_t)active;'),
    ('one way only', 'for (int w = 0; w < GCOPY_WAYS; w++) {\n        GCopyLine *e', 'for (int w = 0; w < 1; w++) {\n        GCopyLine *e'),
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
                caught = code != 0
                print('%-32s %s' % (name, 'caught' if caught else 'MISSED'))
                bad += not caught
            return 1 if bad else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())
