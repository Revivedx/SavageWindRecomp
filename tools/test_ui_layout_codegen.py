"""Synthetic tests for the independently resolved UI draw thunk; no game data required."""
import struct
from types import SimpleNamespace
import game_sites
import patches

base, thunk, native = 0x7100000000, 0x7100000080, 0x7100000020
gates = (0x39440008, 0x7100011f, 0x39440809, 0x3943f80a,
         0x3943fc0b, 0x7a401920, 0x7a400944, 0x7a421964)
image = bytearray(0x100)
branch = 0x14000000 | (((native - thunk) // 4) & 0x3ffffff)
struct.pack_into('<I', image, thunk - base, branch)
struct.pack_into('<8I', image, native - base, *gates)
g = SimpleNamespace(base=base, nso=SimpleNamespace(image=image),
                    sites={'ui_layout_draw': thunk, 'ui_layout_native': native})
code = patches._ui_layout_draw(g, thunk)
assert f'rt_ui_layout_draw(c, f_{native:x}); return;' in code
assert 'c->sp' not in code and 'c->x[' not in code
assert patches.table(g.sites)[thunk] is patches._ui_layout_draw
assert ('ui_layout_draw', 'ui_layout_native') in game_sites.OPTIONAL_GROUPS
for at, wrong in [(thunk, branch | 0x80000000), (thunk, branch + 1),
                  *[(native + 4*i, word ^ 1) for i, word in enumerate(gates)]]:
    saved = struct.unpack_from('<I', image, at - base)[0]
    struct.pack_into('<I', image, at - base, wrong)
    try:
        patches._ui_layout_draw(g, thunk)
    except AssertionError:
        pass
    else:
        raise AssertionError(f'accepted a changed instruction at {at:#x}')
    struct.pack_into('<I', image, at - base, saved)
print('PASS: UI tail thunk dispatch, unchanged guest ABI and rejection of stale branch/gate instructions')
