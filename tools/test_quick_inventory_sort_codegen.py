"""Synthetic entry patches reached through ordinary direct calls and tail calls.

Run with the generator's Python environment and clang; no game files required.
"""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

import gen
import patches

ROOT = Path(__file__).resolve().parent.parent
BASE, TARGET = 0x7000000000, 0x7000000020
RET, NOP = 0xd65f03c0, 0xd503201f


def module(words, locals_):
    fake = SimpleNamespace(text=(0, len(words) * 4), image=struct.pack(f'<{len(words)}I', *words),
                           build_id='00' * 16, syms=[], relocs=[])
    with patch.object(gen, 'Nso', return_value=fake), patch.object(gen, 'eh_frame_extents', return_value=[]):
        return gen.Module('main', 'synthetic.nso', BASE, simd='c', locals_=locals_)


for site, word, restore, hook in (
        ('quick_sort_enter', 0xfc1d0fe8, 0xfc4307e8, 'rt_quick_inventory_sort_begin'),
        ('quick_sort_menu', 0xa9be7bfd, 0xa8c27bfd, 'rt_quick_inventory_sort_menu')):
    for locals_ in (None, gen.Locals()):
        for link in (False, True):
            branch = (0x94000000 if link else 0x14000000) | ((TARGET - BASE) // 4)
            words = [branch, RET] + [NOP] * 6 + [word, restore, RET]
            m = module(words, locals_)
            m.patches = patches.table({site: TARGET})
            refs = set()
            caller = '\n'.join(m._emit_body(BASE, BASE + (8 if link else 4), set(), {}, refs, 'plain'))
            callee = '\n'.join(m._emit_body(TARGET, TARGET + 12, set(), {}, set(), 'plain'))
            assert TARGET in refs and f'{gen.fname(TARGET)}(c)' in caller
            assert 'rt_dispatch(c)' not in caller
            assert callee.count(f'{hook}(c)') == 1
            # Every replaced instruction is checked, rather than silently
            # applying a 1.9.0 layout to a different prologue.
            m.nso.image = bytes(len(m.nso.image))
            try:
                m.patches[TARGET](m, TARGET)
            except AssertionError:
                pass
            else:
                raise AssertionError('changed prologue was accepted')

            source = f'''
#include <assert.h>
#include <string.h>
#include "cpu.h"
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) {{ (void)a; (void)b; (void)d; }}
static Cpu expected;
static int calls;
void rt_quick_drop_begin(Cpu *c) {{ assert(!memcmp(c, &expected, sizeof *c)); }}
void rt_quick_drop_menu(Cpu *c) {{ assert(!memcmp(c, &expected, sizeof *c)); }}
void {hook}(Cpu *c) {{ assert(!memcmp(c, &expected, sizeof *c)); calls++; }}
void {gen.fname(TARGET)}(Cpu *c);
{caller}
{callee}
int main(void) {{
    _Alignas(16) unsigned char stack[256] = {{0}};
    Cpu c = {{0}};
    for (int i = 0; i < 31; i++) c.x[i] = 0x100 + i;
    c.sp = (uintptr_t)(stack + sizeof stack);
    c.v[8] = 0x123456789abcdef0ull;
    c.nzcv = 0xa0000000;
    c.pc = 0x1234;
    expected = c;
    expected.x[30] = {BASE + 4 if link else 0x100 + 30:#x}ull;
    {gen.fname(BASE)}(&c);
    assert(calls == 1);
    assert(!memcmp(&c, &expected, sizeof c));
}}
'''
            with tempfile.TemporaryDirectory(prefix='quick-sort-codegen-') as directory:
                exe = Path(directory) / 'check'
                subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-fno-strict-aliasing',
                                '-I', str(ROOT / 'runtime'), '-x', 'c', '-', '-o', str(exe), '-lm'],
                               input=source, text=True, check=True)
                subprocess.run([str(exe)], check=True)

print('ok: entry patch checks, direct BL/B coverage, original stack/register/FP/flags with and without locals')
