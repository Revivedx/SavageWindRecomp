"""Synthetic tests for runtime detours of direct quick-replacement calls.

Run with the generator's Python environment; no game files are required.
"""
import struct
from types import SimpleNamespace
from unittest.mock import patch

import gen

BASE = 0x7000000000
BUILD = 'cd57b23fa4bbad65'
TARGETS = {BASE + a for a in (0xbe21e0, 0xe68168, 0xe27c2c, 0xe78a30, 0xe7a7f0, 0xbe1e30, 0x13ed264)}


def module(name='main', build=BUILD, base=BASE):
    fake = SimpleNamespace(text=(0, 4), image=struct.pack('<I', 0xd65f03c0),
                           build_id=build, syms=[], relocs=[])
    with patch.object(gen, 'Nso', return_value=fake), \
            patch.object(gen, 'eh_frame_extents', return_value=[]):
        m = gen.Module(name, 'synthetic.nso', base, simd='c')
    # Enclose the synthetic caller and all target offsets in the module's text.
    m.hi = base + 0x2000000
    return m


def branch(m, caller, target, link):
    opcode = 0x94000000 if link else 0x14000000
    word = opcode | (((target - caller) // 4) & 0x3ffffff)
    return m.lifter.lift(word, caller)


def body(m, target, link, caller=BASE + 0x1000):
    ins = branch(m, caller, target, link)
    m.cache = {caller: ins, caller + 4: m.lifter.lift(0xd65f03c0, caller + 4),
               caller + 8: m.lifter.lift(0xd65f03c0, caller + 8),
               target: m.lifter.lift(0xd65f03c0, target)}
    refs = set()
    text = '\n'.join(m._emit_body(caller, caller + (8 if link else 4),
                                  set(), {}, refs, 'plain'))
    return text, refs, ins


m = module()
assert m.runtime_dispatch_targets == TARGETS
for name, build in [('main', 'badbuild'), ('main', 'cd57b23fa4bbad64'),
                    ('sdk', BUILD), ('subsdk0', BUILD)]:
    other = module(name, build)
    assert not other.runtime_dispatch_targets, (name, build)
    text, refs, _ = body(other, BASE + 0xe68168, True)
    assert 'rt_dispatch(c)' not in text
    assert 'f_7000e68168(c)' in text and BASE + 0xe68168 in refs
# Offsets must be relocated from the module's base.
assert module(base=BASE + 0x10000000).runtime_dispatch_targets == \
    {t + 0x10000000 for t in TARGETS}

for target in TARGETS:
    for link in (False, True):
        text, refs, ins = body(m, target, link)
        assert f'c->pc = {gen.hx(target)};' in text
        assert 'rt_dispatch(c)' in text
        assert f'{gen.fname(target)}(c)' not in text
        assert target not in refs
        if link:
            assert f'c->x[30] = {gen.hx(BASE + 0x1004)};' in text
            single_refs = set()
            single = '\n'.join(m.render_single(ins, BASE + 0x1000,
                                               lambda t: 'UNEXPECTED_JUMP', single_refs))
            assert 'rt_dispatch(c)' in single and not single_refs
            assert f'c->x[30] = {gen.hx(BASE + 0x1004)};' in single
        else:
            assert '__attribute__((musttail)) return rt_dispatch(c);' in text
            assert 'c->x[30] =' not in text

# Adjacent ordinary targets retain direct calls and tail calls.
ordinary = BASE + 0xe6816c
for link in (False, True):
    text, refs, ins = body(m, ordinary, link)
    assert 'rt_dispatch(c)' not in text
    assert f'{gen.fname(ordinary)}(c)' in text and ordinary in refs
    if link:
        refs = set()
        single = '\n'.join(m.render_single(ins, BASE + 0x1000, lambda t: '', refs))
        assert 'rt_dispatch(c)' not in single and ordinary in refs
    else:
        assert '__attribute__((musttail)) return f_7000e6816c(c);' in text

# Intra-function branches remain labels, even at a detoured entry address.
caller = BASE + 0xe68164
text, refs, _ = body(m, BASE + 0xe68168, True, caller)
# BL still dispatches; label optimization applies only to B, never calls.
assert 'rt_dispatch(c)' in text
m.cache = {caller: branch(m, caller, caller + 4, False),
           caller + 4: m.lifter.lift(0xd65f03c0, caller + 4)}
text = '\n'.join(m._emit_body(caller, caller + 8, {caller + 4}, {}, set(), 'plain'))
assert 'goto L_7000e68168;' in text and 'rt_dispatch(c)' not in text
print('ok: exact build/module gating, relocated targets, BL/B dispatch, LR, ordinary/local branches')

# The upstream register-local optimization must still spill/reload at these
# dispatched calls, just as it does at external calls.
local_module = module()
local_module.locals = gen.Locals()
for target in TARGETS:
    for link in (False, True):
        text, refs, _ = body(local_module, target, link)
        assert 'rt_dispatch(c)' in text and target not in refs
        assert f'{gen.fname(target)}(c)' not in text
print('ok: replacement detours with register locals enabled')
