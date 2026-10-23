# pe_no_cfg.py <exe>: clear Control Flow Guard (IMAGE_DLLCHARACTERISTICS_GUARD_CF) in a Windows exe's header.
# PyInstaller's bootloader is built with CFG, which then applies to the whole process: unicorn's code generator
# (tools/recomp/gen.py, jump tables) longjmps out of the code it generated, CFG takes that for an attack and ends the process
# (0xC0000409 in VCRUNTIME140.dll) before anything is written. python.exe has no CFG; nor has swgen after this.
import struct
import sys

GUARD_CF = 0x4000

path = sys.argv[1]
b = bytearray(open(path, 'rb').read())
pe = struct.unpack_from('<I', b, 0x3C)[0]
if b[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', b, pe + 24)[0] != 0x20B:
    sys.exit(f'{path}: not a PE32+ image')
off = pe + 24 + 70  # optional header: DllCharacteristics
dc = struct.unpack_from('<H', b, off)[0]
if dc & GUARD_CF:
    struct.pack_into('<H', b, off, dc & ~GUARD_CF)
    open(path, 'wb').write(b)
print(f'{path}: DllCharacteristics {dc:#06x} -> {dc & ~GUARD_CF:#06x}')
