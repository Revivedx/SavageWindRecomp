"""Activation-hook instruction guard; synthetic instructions only."""
import struct
import sys
from pathlib import Path
from types import SimpleNamespace
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import patches
base=0x7000000000
for offset in (0x100,-0x100):
    word=0x94000000|((offset//4)&0x3ffffff)
    gen=SimpleNamespace(base=base,nso=SimpleNamespace(image=struct.pack("<I",word)))
    code=patches._gyro_scene_actor(gen,base)
    assert f"f_{base+offset:x}(c)" in code
    assert f"c->x[30]={base+4:#x}ull" in code
    assert code.index(f"f_{base+offset:x}(c);")<code.index("rt_gyro_scene_actor(c")
gen.nso.image=struct.pack("<I",0xd503201f)
try:
    patches._gyro_scene_actor(gen,base)
except AssertionError:
    pass
else:
    raise AssertionError("unexpected instruction accepted")
print("activation hook: forward/backward BL, link register, call order and opcode rejection PASS")
