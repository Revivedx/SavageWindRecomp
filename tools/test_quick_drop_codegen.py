"""Checked native drop hooks, using synthetic AArch64 and no game files."""
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
for actor_call in (False, True):
    for locals_ in (None, gen.Locals()):
        for link in (False, True):
            branch = (0x94000000 if link else 0x14000000) | ((TARGET - BASE) // 4)
            body = [0xa9bf7bfd, 0xd63f0100, 0xa8c17bfd, RET] if actor_call else [0xd10183ff, 0x910183ff, RET]
            words = [branch, RET] + [NOP] * 6 + body
            image = struct.pack(f'<{len(words)}I', *words)
            fake = SimpleNamespace(text=(0, len(image)), image=image, build_id='00' * 16, syms=[], relocs=[])
            with patch.object(gen, 'Nso', return_value=fake), patch.object(gen, 'eh_frame_extents', return_value=[]):
                m = gen.Module('main', 'synthetic.nso', BASE, simd='c', locals_=locals_)
            site = 'quick_drop_actor_call' if actor_call else 'quick_drop_tick'
            address = TARGET + (4 if actor_call else 0)
            m.patches = patches.table({site: address})
            refs = set()
            caller = '\n'.join(m._emit_body(BASE, BASE + (8 if link else 4), set(), {}, refs, 'plain'))
            callee = '\n'.join(m._emit_body(TARGET, TARGET + len(body) * 4, set(), {}, set(), 'plain'))
            assert TARGET in refs and 'rt_dispatch(c)' not in caller
            assert callee.count('rt_dispatch(c)') == int(actor_call)
            m.nso.image = bytes(len(image))
            try:
                m.patches[address](m, address)
            except AssertionError:
                pass
            else:
                raise AssertionError('changed instruction accepted')
            source = f'''
#include <assert.h>
#include <string.h>
#include "cpu.h"
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) {{ (void)a; (void)b; (void)d; }}
static Cpu expected;
static int ticks, begins, dispatches, ends;
static uint64_t token;
void quick_drop_action_tick(Cpu *c) {{ assert(!memcmp(c, &expected, sizeof *c)); ticks++; }}
uint64_t rt_quick_drop_actor_begin(uint64_t actor, uint64_t method) {{
    assert(actor == 0x1111 && method == 0x5555); begins++; return token;
}}
void rt_dispatch(Cpu *c) {{
    assert(c->x[0] == 0x1111 && c->pc == 0x5555 && c->x[30] == {TARGET + 8:#x}ull);
    dispatches++; c->x[0] = 1; c->x[3] = 0xdeadbeef; c->v[0] = 0x2222; c->nzcv = 0x70000000; c->pc = 0x7777;
}}
void rt_quick_drop_actor_end(uint64_t seen, uint64_t actor, uint64_t result) {{
    assert(seen == token && actor == 0x1111 && result == 1); ends++;
}}
void {gen.fname(TARGET)}(Cpu *c);
{caller}
{callee}
int main(void) {{
    for (int pending = 0; pending < 2; pending++) {{
        _Alignas(16) unsigned char stack[256] = {{0}};
        Cpu c = {{0}};
        for (int i = 0; i < 31; i++) c.x[i] = 0x100 + i;
        c.x[0] = 0x1111; c.x[8] = 0x5555;
        c.sp = (uintptr_t)(stack + sizeof stack); c.nzcv = 0xa0000000; c.pc = 0x1234;
        expected = c; expected.x[30] = {BASE + 4 if link else 0x100 + 30:#x}ull;
        token = pending ? 0x42 : 0;
        {gen.fname(BASE)}(&c);
        if ({int(actor_call)}) {{
            expected.x[0] = 1; expected.x[3] = 0xdeadbeef; expected.v[0] = 0x2222;
            expected.nzcv = 0x70000000; expected.pc = 0x7777;
            assert(dispatches == pending + 1 && begins == dispatches && ends == dispatches);
        }} else assert(ticks == pending + 1);
        assert(!memcmp(&c, &expected, sizeof c));
    }}
}}
'''
            with tempfile.TemporaryDirectory(prefix='quick-drop-codegen-') as directory:
                exe = Path(directory) / 'check'
                subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-fno-strict-aliasing',
                                '-I', str(ROOT / 'runtime'), '-x', 'c', '-', '-o', str(exe), '-lm'],
                               input=source, text=True, check=True)
                subprocess.run([str(exe)], check=True)
print('ok: direct tick hooks, checked BLR actor observation, native result/LR/FP/flags with and without locals')
