"""Symbolize guest addresses: sym.py <addr>... [--dis N]"""
import bisect
import struct
import subprocess
import sys

import capstone
from nso import Nso

MODS = [('main', 0x7000000000), ('sdk', 0x7004000000)]
_cache = {}


def mod(name):
    if name not in _cache:
        n = Nso(f'../../extracted/exefs/{name}')
        syms = sorted((s[3], s[0]) for s in n.syms if s[2] and (s[1] & 0xF) == 2)
        _cache[name] = (n, syms, [a for a, _ in syms])
    return _cache[name]


def demangle(s):
    return subprocess.run(['c++filt'], input=s, capture_output=True, text=True).stdout.strip()


def lookup(addr):
    for name, base in MODS:
        n, syms, addrs = mod(name)
        if base <= addr < base + n.size:
            off = addr - base
            i = bisect.bisect_right(addrs, off) - 1
            if i >= 0:
                return name, base, off, syms[i][1], off - syms[i][0]
            return name, base, off, None, 0
    return None


if __name__ == '__main__':
    dis = 0
    args = sys.argv[1:]
    if '--dis' in args:
        i = args.index('--dis')
        dis = int(args[i + 1])
        del args[i:i + 2]
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for a in args:
        addr = int(a, 16)
        r = lookup(addr)
        if not r:
            print(f'{addr:x}: ?')
            continue
        name, base, off, sym, delta = r
        print(f'{addr:x}: {name}+{off:x}  {demangle(sym) if sym else "?"}+{delta:x}')
        if dis:
            n = mod(name)[0]
            for ins in md.disasm(bytes(n.image[off:off + 4 * dis]), addr):
                t = ''
                if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
                    rr = lookup(int(ins.op_str[1:], 16))
                    if rr and rr[3]:
                        t = '  ; ' + demangle(rr[3])[:80]
                print(f'    {ins.address:x}: {ins.mnemonic:8} {ins.op_str}{t}')
