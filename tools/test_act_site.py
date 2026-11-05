"""Per-action hook (tools/patches.py _act_query, docs/ACTIONS.md) without game files: made-up instructions only."""
import struct
import sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import patches

STP = 0xA9BE7BFD  # stp x29, x30, [sp, #-0x20]!: the first instruction of both queries
base = 0x7000000000
masks = 0x70016D89B4
gen = SimpleNamespace(base=base, nso=SimpleNamespace(image=struct.pack("<I", STP)),
                      sites={"act_hold": base, "act_trig": base, "act_masks": masks})

for kind, name in ((0, "act_hold"), (1, "act_trig")):
    code = patches.PATCHES[name](gen, base)
    call = f"rt_act_query({kind}, c->x[0], c->x[1], c->x[30], {masks:#x}ull)"
    assert call in code, code
    # an answer of the runtime's returns early with the result in w0 ...
    assert "if (r >= 0) { c->x[0] = (uint64_t)r; return; }" in code, code
    # ... and otherwise the replaced stp runs (the frame is pushed, 0x20 bytes) before the game's own code
    assert code.index("return;") < code.index("c->sp = ea;"), code
    assert "c->sp - 0x20" in code and "ST64(ea, c->x[29]); ST64(ea + 8, c->x[30])" in code, code
    assert code.startswith("{") and "}" in code and code.count("{") == code.count("}"), code

assert "held" in patches.PATCHES["act_hold"](gen, base) and "pressed" in patches.PATCHES["act_trig"](gen, base)

for bad in (0xA9BF7BFD, 0xD503201F, 0xA9BD7BFD):  # stp #-0x10! / nop / stp #-0x30!
    gen.nso.image = struct.pack("<I", bad)
    try:
        patches.PATCHES["act_hold"](gen, base)
    except AssertionError:
        pass
    else:
        raise AssertionError(f"unexpected instruction {bad:#x} accepted")

# a game without the sites (an optional group left out) has no patch for them
assert patches.table({"pc_map_focus": 0x7000001000}).get(base) is None
print("action hook: both queries, early return, the replaced stp, the mask table and opcode rejection PASS")
