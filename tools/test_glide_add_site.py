"""Both fadd mirrors of the glide-decay sites: their exact synthetic 5-word patterns and patches.

No game files and no real NSO: a SimpleNamespace holds five made-up instruction words at each
site, and game_sites.Main locates them in a made-up text section. All five words are needed: the
trailing four words (b.eq / ldr / ldr / fadd) are identical at both sites.
"""
import struct
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import game_sites as gs
import patches

FADD = 0x1e212800  # fadd s0, s0, s1: the word both patches replace
BASE = 0x7000c86000  # a made-up module base; both sites fit in its 0x1000-byte .text
SITES = (
    ("glide_overspeed_add_181c", 0x7000c86708, 0x7000c86718,
     (0x1e2e2000, 0x540000a5, 0xf9403668, 0xbd400101, FADD)),  # the fcmp that tells this site from the other
    ("glide_overspeed_add_1824", 0x7000c867cc, 0x7000c867dc,
     (0x1e2d2000, 0x540000a5, 0xf9403668, 0xbd400101, FADD)),
)
checks = 0


def check(cond, what):
    global checks
    if not cond:
        sys.exit(f"FAIL: {what}")
    checks += 1


image = bytearray(0x1000)  # NOPs, except for the two exact patterns
for _, start, _, words in SITES:
    struct.pack_into("<5I", image, start - BASE, *words)
nso = SimpleNamespace(image=bytes(image), text=(0, 0x1000))
main = gs.Main(nso, BASE)


def patched(name, target, word):
    """Run the registered patch against the synthetic module with `word` at the fadd."""
    data = bytearray(nso.image)
    struct.pack_into("<I", data, target - BASE, word)
    gen = SimpleNamespace(base=BASE, nso=SimpleNamespace(image=bytes(data)))
    return patches.PATCHES[name](gen, target)


for name, start, target, words in SITES:
    site = next((s for s in gs.SITES if s.name == name), None)
    check(site is not None, f"{name}: site registered")
    check(site.pattern == " ".join(f"{w:08x}" for w in words), f"{name}: registered 5-word pattern")
    check(site.at == 4, f"{name}: the fadd is the fifth word")
    check(main.locate(site) == (target, 1), f"{name}: pattern finds one instruction at {target:#x}")
    check(gs.KNOWN["1.9.0"][name] == target, f"{name}: 1.9.0 address")

    code = patched(name, target, FADD)
    check("extern float rt_glide_overspeed_add(float, float);" in code, f"{name}: helper declaration")
    check("rt_glide_overspeed_add(v.f, d.f)" in code, f"{name}: helper call with s0 / s1")
    check("(uint32_t)c->v[0]" in code and "(uint32_t)c->v[1]" in code and "c->v[0] = v.u" in code,
          f"{name}: v0 in, v1 in, result in v0")
    check("c->sp" not in code and "c->x[" not in code, f"{name}: no integer register or stack change")
    check(f"/* {target:#x}: fadd s0, s0, s1" in code, f"{name}: replaced word is documented")

    for bad in (0x1e213800, 0xd503201f, words[3]):  # fsub / nop / the store word
        try:
            patched(name, target, bad)
        except AssertionError:
            checks += 1
        else:
            sys.exit(f"FAIL: {name}: accepted wrong word {bad:#x}")

print(f"ok: {checks} checks")
