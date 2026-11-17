#!/usr/bin/env python3
"""Fuzz the texture write-watch change detection (runtime/nvn/vulkan/textures.c, between TWW-BEGIN and TWW-END).

The real code is extracted and compiled against a model of Windows' write watch (one flag per 4K page, set by every
write, cleared by a query that resets) and of the heap copy (exec.c) that resets the flags of the spans it holds and
hands the pages over. No game, GPU or Windows is needed.

Checked, over random writes, textures that overlap or share their end pages, the heap copy's resets (also the
overflowing kind), pages that turn into geometry, and queries that fail:
  1. a texture whose check says "unchanged" has the contents it was last read with (no missed write);
  2. the heap copy sees every write to a page of geometry (a texture's reset never hides one from it);
  3. two armed textures never hold the same page;
  4. the page bit helpers agree with a plain array.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import os

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'runtime/nvn/vulkan/textures.c'
VKH = ROOT / 'runtime/nvn/vulkan/vk.h'

FIXTURE = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUEST_LO 0x10000000ull
#define NPG 4096ull
#define GUEST_HI (GUEST_LO + NPG * 4096ull)

@TEXWW@

@HASH_FULL@

// ---- the write watch: a flag per page
static uint8_t F[NPG];
static uint8_t *guest;  // the guest memory: page p at guest + p * 4096
static int ww_fail;     // 1: every query fails (no write watch)
static uint8_t geo[NPG];  // the heap copy's geometry pages
static uint64_t hmirror_ver[NPG];  // the page version the copy holds, or ~0: known stale
static uint64_t pver[NPG];  // a page's version: bumped by every write

static long plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, size_t max) {
    if (ww_fail) return -1;
    size_t n = 0;
    for (uint64_t p = (addr - GUEST_LO) >> 12; p < (addr + size - GUEST_LO + 4095) >> 12; p++)
        if (F[p]) {
            if (n < max) { pages[n] = GUEST_LO + p * 4096; F[p] = 0; }
            n++;
        }
    return n >= max ? -1 : (long)n;
}

static int plat_any_written(uint64_t addr, uint64_t size) {
    if (ww_fail) return -1;
    for (uint64_t p = (addr - GUEST_LO) >> 12; p < (addr + size - GUEST_LO + 4095) >> 12; p++)
        if (F[p]) return 1;
    return 0;
}

static bool vk_heap_geometry_any(uint64_t lo, uint64_t hi) {
    for (uint64_t p = (lo - GUEST_LO) >> 12; p < (hi - GUEST_LO + 4095) >> 12; p++)
        if (geo[p]) return true;
    return false;
}

@TWW@

// ---- the model
static uint64_t rng = 1;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void write_bytes(uint64_t off, uint64_t n, bool same) {  // (a write: the flags go up whether or not it changed anything)
    for (uint64_t i = 0; i < n; i++) if (!same) guest[off + i] = (uint8_t)rnd();
    for (uint64_t p = off >> 12; p <= (off + n - 1) >> 12; p++) { F[p] = 1; pver[p]++; }
}

typedef struct {
    TexWW ww;
    uint64_t off, total;  // storage, from the start of the guest memory
    uint64_t snap;        // the contents as last read
    bool uploaded;
    bool dynamic;
} Tex;

static uint64_t tex_hash(const Tex *t) { return hash_full(guest + t->off, t->total); }

static uint64_t fails, region;

// what upload_texture does with the write watch, minus the sampling
static void check(Tex *t, uint64_t frame, int validate) {
    uint64_t addr = GUEST_LO + t->off;
    const uint8_t *mem = guest + t->off;
    if (t->uploaded) {
        int st = tww_check(&t->ww, mem, addr, t->total);
        if (st == 0) {
            if (tex_hash(t) != t->snap) { fails++; if (fails < 10) printf("missed write: texture %llx+%llx armed %d\n", (unsigned long long)t->off, (unsigned long long)t->total, t->ww.token != 0); }
            return;
        }
    }
    tww_arm(&t->ww, mem, addr, t->total, t->dynamic, frame);
    t->snap = tex_hash(t);
    t->uploaded = true;
    (void)validate;
}

#define NT 24
static Tex tex[NT];

static void heap_copy_reset(uint64_t lo_page, uint64_t n_pages) {  // exec.c hc_list_begin for one span
    uint64_t lo = GUEST_LO + lo_page * 4096, size = n_pages * 4096;
    static uint64_t list[64];  // (small: it overflows now and then)
    if (plat_any_written(lo, size) == 0) return;
    long n = plat_written_pages(lo, size, list, 64);
    if (n < 0) {
        // the list overflowed: pages it held were reset, all of the span is stale and handed over
        vk_tex_range_written(lo, size);
        for (uint64_t p = lo_page; p < lo_page + n_pages; p++) if (geo[p]) hmirror_ver[p] = ~0ull;  // stale: recopied
        return;
    }
    vk_tex_pages_written(list, n);
    for (long k = 0; k < n; k++) { uint64_t p = (list[k] - GUEST_LO) >> 12; hmirror_ver[p] = ~0ull; }
}

static void heap_copy_verify(uint64_t lo_page, uint64_t n_pages) {  // every geometry page of the span queried, whose copy is out of date, is marked so
    for (uint64_t p = lo_page; p < lo_page + n_pages; p++)
        if (geo[p] && hmirror_ver[p] != ~0ull && hmirror_ver[p] != pver[p]) {
            fails++;
            if (fails < 10) printf("heap copy missed a write to geometry page %llu\n", (unsigned long long)p);
            hmirror_ver[p] = pver[p];
        }
}

static void heap_copy_refresh(void) {  // the next list copies the stale pages over
    for (uint64_t p = 0; p < NPG; p++) if (geo[p] && hmirror_ver[p] == ~0ull) hmirror_ver[p] = pver[p];
}

static void check_claims(void) {
    static uint8_t n[NPG];
    memset(n, 0, sizeof n);
    for (int i = 0; i < NT; i++)
        if (tex[i].ww.token)
            for (uint64_t p = tex[i].ww.page0; p < tex[i].ww.page0 + tex[i].ww.npages; p++) {
                if (n[p]++) { fails++; if (fails < 10) printf("page %llu held twice\n", (unsigned long long)p); }
                if (tww_claim[p] != tex[i].ww.token) { fails++; if (fails < 10) printf("claim of page %llu not the texture's\n", (unsigned long long)p); }
            }
}

static void test_bits(void) {
    static uint8_t ref[NPG];
    memset(ref, 0, sizeof ref);
    assert(tww_dirty);
    tww_dirty_clear(0, NPG);
    for (int i = 0; i < 40000; i++) {
        uint64_t first = rnd() % NPG, n = 1 + rnd() % (rnd() % 4 ? 70 : 600);
        if (first + n > NPG) n = NPG - first;
        switch (rnd() % 3) {
        case 0: { uint64_t a = GUEST_LO + first * 4096; vk_tex_pages_written(&a, 1); ref[first] = 1; break; }
        case 1: tww_dirty_clear(first, n); for (uint64_t p = first; p < first + n; p++) ref[p] = 0; break;
        case 2: {  // (from inside a page to inside another)
            uint64_t a = GUEST_LO + first * 4096 + rnd() % 4096, size = 1 + rnd() % (n * 4096);
            if (a + size > GUEST_HI) size = GUEST_HI - a;
            vk_tex_range_written(a, size);
            for (uint64_t p = (a - GUEST_LO) >> 12; p <= (a + size - 1 - GUEST_LO) >> 12; p++) ref[p] = 1;
            break;
        }
        }
        uint64_t qf = rnd() % NPG, qn = 1 + rnd() % 200;
        if (qf + qn > NPG) qn = NPG - qf;
        bool any = false;
        for (uint64_t p = qf; p < qf + qn; p++) any |= ref[p] != 0;
        if (tww_dirty_any(qf, qn) != any) { fails++; if (fails < 10) printf("tww_dirty_any disagrees at %llu+%llu\n", (unsigned long long)qf, (unsigned long long)qn); }
    }
    for (uint64_t p = 0; p < NPG; p++)
        if (((tww_dirty[p >> 6] >> (p & 63)) & 1) != ref[p]) { fails++; printf("page bit %llu differs\n", (unsigned long long)p); break; }
}

int main(int argc, char **argv) {
    int seeds = argc > 1 ? atoi(argv[1]) : 100;
    int steps = argc > 2 ? atoi(argv[2]) : 20000;
    guest = calloc(NPG, 4096);
    for (int seed = 1; seed <= seeds && !fails; seed++) {
        rng = (uint64_t)seed * 0x9E3779B97F4A7C15ull | 1;
        memset(F, 0, sizeof F); memset(geo, 0, sizeof geo); memset(hmirror_ver, 0, sizeof hmirror_ver); memset(pver, 0, sizeof pver);
        for (uint64_t i = 0; i < NPG * 4096; i += 8) *(uint64_t *)(guest + i) = rnd();
        free(tww_claim); tww_claim = NULL; free(tww_dirty); tww_dirty = NULL; tww_token_next = 0;
        // textures: end to end (sharing their end pages, here and there a gap), or anywhere and overlapping (seed % 3 == 0);
        // page aligned or not; 1-300 pages
        uint64_t cursor = (rnd() % 4) * 4096 + rnd() % 4096;
        region = 0;
        for (int i = 0; i < NT; i++) {
            memset(&tex[i], 0, sizeof tex[i]);
            uint64_t pages = 1 + rnd() % (seed % 5 == 0 ? 8 : seed % 3 == 0 ? 300 : 100);
            tex[i].total = pages * 4096 + (rnd() % 3 ? rnd() % 4096 : 0) - (rnd() % 2 ? 0 : 512);
            if (seed % 3 == 0) tex[i].off = (rnd() % 1024) * 4096 + (rnd() % 3 ? rnd() % 4096 : 0);
            else tex[i].off = cursor, cursor += tex[i].total + (rnd() % 3 ? 0 : rnd() % 20000);
            if (tex[i].off + tex[i].total > NPG * 4096) tex[i].total = NPG * 4096 - tex[i].off;
            if (tex[i].off + tex[i].total > region) region = tex[i].off + tex[i].total;
            tex[i].dynamic = rnd() % 4 == 0;
        }
        region = (region >> 12) + 2;  // pages
        ww_fail = 0;
        for (int s = 0; s < steps; s++) {
            uint64_t frame = (uint64_t)s;
            switch (rnd() % 12) {
            case 0: case 1: case 2: {  // the CPU writes into a texture, or anywhere
                Tex *t = &tex[rnd() % NT];
                uint64_t off = rnd() % 2 ? t->off + rnd() % t->total : rnd() % (region * 4096ull);
                uint64_t n = 1 + rnd() % (rnd() % 8 == 0 ? 20000 : 300);
                if (off + n > NPG * 4096) n = NPG * 4096 - off;
                write_bytes(off, n, rnd() % 4 == 0);
                break;
            }
            case 3: case 4: case 5: case 6: case 7:
                check(&tex[rnd() % NT], frame, 0);
                break;
            case 8: {  // the heap copy resets a span
                uint64_t lo = rnd() % region, n = 1 + rnd() % (rnd() % 4 ? 64 : 700);
                if (lo + n > NPG) n = NPG - lo;
                heap_copy_reset(lo, n);
                heap_copy_verify(lo, n);
                heap_copy_refresh();
                break;
            }
            case 9: {  // pages turn into geometry (the copy takes their contents when first fetched)
                if (rnd() % 40) break;  // (geometry is a small part of the heap, and the set only grows)
                uint64_t lo = rnd() % region, n = 1 + rnd() % 3;
                for (uint64_t p = lo; p < lo + n && p < NPG; p++) { geo[p] = 1; hmirror_ver[p] = pver[p]; }
                break;
            }
            case 10: ww_fail = rnd() % 16 == 0; break;
            case 11: if (rnd() % 8 == 0) { Tex *t = &tex[rnd() % NT]; tww_release(&t->ww); t->ww.retry = 0; } break;
            }
            if (s % 64 == 0) check_claims();
        }
    }
    test_bits();
    if (fails) { printf("FAIL: %llu\n", (unsigned long long)fails); return 1; }
    printf("ok: %d seeds x %d steps; stats skipped %llu written %llu armed %llu refused %llu\n", seeds, steps,
           (unsigned long long)tww_stat_skip, (unsigned long long)tww_stat_written, (unsigned long long)tww_stat_armed,
           (unsigned long long)tww_stat_refused);
    return 0;
}
'''


def between(text, a, b):
    i = text.index(a) + len(a)
    return text[i:text.index(b, i)]


def hash_full_source(text):
    start = text.index('static uint64_t hash_full(const uint8_t *p, uint64_t n) {')
    pos = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[start:pos]


# Deliberate breakages of the extracted code: each one has to be caught (--self-check)
MUTANTS = {
    'no geometry check': ('vk_heap_geometry_any(lo, hi) ||', ''),
    'heap copy pages not handed over': ('if (pages[i] >= GUEST_LO && pages[i] < GUEST_HI) tww_dirty_set((pages[i] - GUEST_LO) >> 12);', '(void)0;'),
    'heap copy ranges not handed over': ('for (uint64_t p = lo < hi ? (lo - GUEST_LO) >> 12 : 1, e = lo < hi ? (hi - 1 - GUEST_LO) >> 12 : 0; p <= e; p++) tww_dirty_set(p);', '(void)0;'),
    'edges not hashed': ('return tww_edge(mem, addr, total, lo, hi) != w->edge;', 'return 0;'),
    'pages shared between textures': ('if (tww_claim[p]) { w->retry', 'if (0) { w->retry'),
    'dirty bits not read': ('if (r > 0 || tww_dirty_any(w->page0, w->npages)) return 1;', 'if (r > 0) return 1;'),
}


def build_and_run(text, vk, seeds, steps, mutant=None):
    texww = re.search(r'typedef struct TexWW \{.*?\} TexWW;', vk, re.S).group(0)
    tww = between(text, '// TWW-BEGIN', '// TWW-END')
    if mutant:
        old, new = MUTANTS[mutant]
        assert tww.count(old) == 1, mutant
        tww = tww.replace(old, new)
    src = FIXTURE.replace('@TEXWW@', texww).replace('@HASH_FULL@', hash_full_source(text)).replace('@TWW@', tww)
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / 'tww.c'
        exe = Path(d) / 'tww'
        c.write_text(src)
        cc = os.environ.get('CC', 'cc')
        subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined', '-Wall', '-Wno-unused-function',
                        '-Wno-unused-variable', '-o', str(exe), str(c), '-lpthread'], check=True)
        return subprocess.run([str(exe), str(seeds), str(steps)], capture_output=mutant is not None, text=True).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', type=int, default=60)
    ap.add_argument('--steps', type=int, default=20000)
    ap.add_argument('--self-check', action='store_true', help='also break the code in known ways: the test has to fail each time')
    args = ap.parse_args()
    text = SRC.read_text()
    vk = VKH.read_text()
    if build_and_run(text, vk, args.seeds, args.steps):
        raise SystemExit(1)
    if args.self_check:
        for name in MUTANTS:
            if build_and_run(text, vk, args.seeds, args.steps, name) == 0:
                raise SystemExit('the test does not notice: ' + name)
            print('caught:', name)


if __name__ == '__main__':
    main()
