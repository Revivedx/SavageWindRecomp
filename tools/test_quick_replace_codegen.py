"""Execute checked replacement hooks with synthetic guest code, with/without locals."""
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
BASE, RET = 0x7000000000, 0xd65f03c0
MOV7, ZERO0 = 0x52800f67, 0xd2800000
CASES = [
    ('tick_call', [0x940004f3, RET], {}),
    ('fill_call', [0x97fffdbd, RET], {}),
    ('full_info_call', [0x94224eaf, RET], {}),
    ('ui_draw', [0x17ee0583], {'replace_ui_draw_native': BASE + 0x1000}),
    ('popup', [0xd10103ff, 0x910103ff, RET], {}),
    ('item_popup', [0xf9400408, RET], {}),
    ('receive', [0xd106c3ff, MOV7, 0x9106c3ff, RET], {}),
    ('card_close_gate', [0xd100c3ff, MOV7, 0x9100c3ff, RET], {}),
    ('card_action', [0xd10243ff, MOV7, 0x910243ff, RET], {}),
    ('pane_draw', [0x39416008, MOV7, RET], {}),
    ('anim_advance', [0xbd405001, MOV7, RET], {}),
    ('query', [0xa9bf7bfd, ZERO0, 0xa8c17bfd, RET], {'replace_query_return': BASE + 12}),
    ('chest_update', [0xd101c3ff, ZERO0, 0x9101c3ff, RET], {'replace_chest_return': BASE + 12}),
    ('toast_update', [0xa9be7bfd, 0xa8c27bfd, RET], {'replace_toast_return': BASE + 8}),
    ('toast_update', [0xa9be7bfd, 0xa8c27bfd, 0x17ffffb3], {'replace_toast_tail_a': BASE + 8}),
    ('toast_update', [0xa9be7bfd, 0xa8c27bfd, 0x17fffd8f], {'replace_toast_tail_b': BASE + 8}),
]

for locals_ in (None, gen.Locals()):
    for name, words, extra in CASES:
        image = struct.pack(f'<{len(words)}I', *words)
        fake = SimpleNamespace(text=(0, len(image)), image=image, build_id='00' * 16, syms=[], relocs=[])
        with patch.object(gen, 'Nso', return_value=fake), patch.object(gen, 'eh_frame_extents', return_value=[]):
            m = gen.Module('main', 'synthetic.nso', BASE, simd='c', locals_=locals_)
        m.sites = {'replace_' + name: BASE, **extra}
        m.patches = patches.table(m.sites)
        body = '\n'.join(m._emit_body(BASE, BASE + len(image), set(), {}, set(), 'plain'))
        # Every replaced instruction must fail closed if its opcode changes.
        m.nso.image = bytes(len(image))
        for address, emit in m.patches.items():
            try:
                emit(m, address)
            except AssertionError:
                pass
            else:
                raise AssertionError(f'{name}: changed instruction accepted')
        m.nso.image = image
        expected, helpers = [], []
        native_effects = 'c->x[0] = 1; c->x[3] = 0xdead; c->v[0] = 0x2222; c->nzcv = 0x70000000;'
        expected_effects = native_effects.replace('c->', 'expected.')
        is_call = name in ('tick_call', 'fill_call', 'full_info_call', 'ui_draw')
        if is_call:
            dest = extra.get('replace_ui_draw_native', m.lift(BASE).target)
            native = f'f_{dest:x}'
            hook = 'rt_quick_replace_' + name.removesuffix('_call')
            expected += [f'expected.x[30] = {BASE + 4:#x}ull;'] if name != 'ui_draw' else []
            helpers += [f'void {native}(Cpu *c) {{ assert(!memcmp(c, &expected, sizeof *c)); native_calls++; {native_effects} }}',
                        f'void {hook}(Cpu *c, GuestFn native) {{ assert(native == {native}); hooks++; native(c); }}']
            expected += [expected_effects]
        elif name == 'query':
            helpers += ['void rt_quick_replace_query(Cpu *c, uint64_t query) { assert(query == before.x[0] && c->x[0] == 0 && c->sp == before.sp); hooks++; }']
            expected += ['expected.x[0] = 0;']
        elif name == 'chest_update':
            helpers += ['unsigned rt_quick_replace_chest_begin(Cpu *c) { assert(!memcmp(c, &before, sizeof *c)); hooks++; return mode; }',
                        'void rt_quick_replace_chest_end(Cpu *c, uint64_t ai, unsigned seen) { assert(ai == before.x[0] && seen == mode && c->x[0] == 0 && c->sp == before.sp); posts++; }']
            expected += ['if (mode) expected.x[0] = 0;', 'assert(posts == (mode != 0));']
        elif name == 'toast_update':
            helpers += ['int rt_quick_replace_toast_begin(Cpu *c) { assert(!memcmp(c, &before, sizeof *c)); hooks++; return mode ? 0x82 : -1; }',
                        'void rt_quick_replace_toast_end(uint64_t toast, int saved) { assert(toast == before.x[0] && saved == (mode ? 0x82 : -1)); posts++; }']
            if words[-1] != RET:
                dest = m.lift(BASE + 8).target
                helpers += [f'void f_{dest:x}(Cpu *c) {{ assert(!memcmp(c, &before, sizeof *c)); native_calls++; {native_effects} }}']
                expected += [expected_effects]
            expected += ['assert(posts == 1);']
        elif name in ('popup', 'item_popup'):
            helpers += [f'void rt_quick_replace_{name}(Cpu *c) {{ assert(!memcmp(c, &before, sizeof *c)); hooks++; }}']
            if name == 'item_popup':
                expected += ['expected.x[8] = LD64(before.x[0] + 8);']
        else:
            hook = {'card_close_gate': 'card_close', 'pane_draw': 'pane', 'anim_advance': 'anim'}.get(name, name)
            helpers += [f'bool rt_quick_replace_{hook}(Cpu *c) {{ assert(!memcmp(c, &before, sizeof *c)); hooks++; return mode; }}']
            expected += ['if (!mode) expected.x[7] = 0x7b;']
            if name == 'pane_draw':
                expected += ['if (!mode) expected.x[8] = LD8(before.x[0] + 0x58);']
            if name == 'anim_advance':
                expected += ['if (!mode) expected.v[1] = LD32(before.x[0] + 0x50);']
        # The original callback checks its incoming CPU before the expected
        # native effects are applied to the comparison snapshot.
        setup_expected = '\n'.join(x for x in expected if x.startswith('expected.x[30]'))
        after_expected = '\n'.join(x for x in expected if not x.startswith('expected.x[30]'))
        source = f'''
#include <assert.h>
#include <stdbool.h>
#include "cpu.h"
volatile int g_fnprof;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) {{ (void)a; (void)b; (void)d; }}
static Cpu before, expected;
static unsigned mode, hooks, posts, native_calls;
{''.join(helpers)}
{body}
int main(void) {{
    for (mode = 0; mode < 4; mode++) {{
        _Alignas(16) unsigned char stack[2048] = {{0}}, item[128];
        memset(item, 0xa5, sizeof item);
        Cpu c; memset(&c, 0x53, sizeof c);
        c.x[0] = (uintptr_t)item; c.sp = (uintptr_t)(stack + sizeof stack);
        c.x[29] = 0x1234; c.x[30] = 0x9876; c.nzcv = 0xa0000000;
        before = expected = c; hooks = posts = native_calls = 0;
        {setup_expected}
        f_{BASE:x}(&c);
        {after_expected}
        assert(hooks == 1 && !memcmp(&c, &expected, sizeof c));
    }}
}}
'''
        with tempfile.TemporaryDirectory(prefix='quick-replace-codegen-') as directory:
            exe = Path(directory) / 'check'
            subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1', '-fno-strict-aliasing',
                            '-I', str(ROOT / 'runtime'), '-x', 'c', '-', '-o', str(exe), '-lm'],
                           input=source, text=True, check=True)
            subprocess.run([str(exe)], check=True)
print('ok: checked replacement gates, scoped query/chest/toast exits and native callbacks; LR/FP/SIMD/flags, plain/locals')
