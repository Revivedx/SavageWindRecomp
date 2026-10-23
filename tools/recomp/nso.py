"""NSO loader: decompress segments, build the memory image, parse MOD0/dynamic info."""
import struct
import sys
import lz4.block

DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_JMPREL, DT_RELRSZ, DT_RELR, DT_RELACOUNT, DT_GNU_HASH = 10, 23, 35, 36, 0x6FFFFFF9, 0x6FFFFEF5
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


def header(path):
    """Segment placement from the NSO header alone (nothing decompressed): what tools/layout.py needs."""
    with open(path, 'rb') as f:
        raw = f.read(0x100)
    assert raw[:4] == b'NSO0', path
    seg = [struct.unpack_from('<II', raw, 0x14 + i * 0x10) for i in range(3)]  # (mem_off, size)
    return {'text': seg[0], 'rodata': seg[1], 'data': seg[2], 'bss': struct.unpack_from('<I', raw, 0x3C)[0],
            'build_id': raw[0x40:0x60].hex()}


class Nso:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        assert raw[:4] == b'NSO0', path
        flags = struct.unpack_from('<I', raw, 0xC)[0]
        self.build_id = raw[0x40:0x60].rstrip(b'\0').hex()
        segs = []
        for i in range(3):
            file_off, mem_off, dec_size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
            comp_size = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
            data = raw[file_off:file_off + comp_size]
            if flags & (1 << i):
                data = lz4.block.decompress(data, uncompressed_size=dec_size)
            segs.append((mem_off, data))
        self.bss_size = struct.unpack_from('<I', raw, 0x3C)[0]
        self.text = (segs[0][0], len(segs[0][1]))
        self.rodata = (segs[1][0], len(segs[1][1]))
        self.data = (segs[2][0], len(segs[2][1]))
        end = segs[2][0] + len(segs[2][1]) + self.bss_size
        self.size = (end + 0xFFF) & ~0xFFF
        img = bytearray(self.size)
        for off, d in segs:
            img[off:off + len(d)] = d
        self.image = img
        self._parse_mod0()

    def u32(self, off): return struct.unpack_from('<I', self.image, off)[0]
    def u64(self, off): return struct.unpack_from('<Q', self.image, off)[0]

    def _parse_mod0(self):
        mod0 = self.u32(4)
        assert self.image[mod0:mod0 + 4] == b'MOD0'
        rel = lambda i: mod0 + struct.unpack_from('<i', self.image, mod0 + 4 + i * 4)[0]
        self.dynamic, self.bss_start, self.bss_end = rel(0), rel(1), rel(2)
        self.eh_start, self.eh_end, self.module_obj = rel(3), rel(4), rel(5)
        dyn = {}
        needed = []
        off = self.dynamic
        while True:
            tag, val = self.u64(off), self.u64(off + 8)
            off += 16
            if tag == DT_NULL:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            dyn.setdefault(tag, val)
        self.dyn = dyn
        strtab = dyn.get(DT_STRTAB, 0)
        self.strtab = strtab
        self.needed = [self.cstr(strtab + n) for n in needed]
        self.syms = []
        if DT_SYMTAB in dyn:
            st = dyn[DT_SYMTAB]
            for i in range(self._nsyms()):
                o = st + i * 24
                name, info, other, shndx = struct.unpack_from('<IBBH', self.image, o)
                value, size = struct.unpack_from('<QQ', self.image, o + 8)
                self.syms.append((self.cstr(strtab + name), info, shndx, value, size))
        self.relocs = []
        for base_tag, size_tag in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
            if base_tag in dyn:
                b, n = dyn[base_tag], dyn[size_tag] // 24
                for i in range(n):
                    r_off, r_info, r_add = struct.unpack_from('<QQq', self.image, b + i * 24)
                    self.relocs.append((r_off, r_info & 0xFFFFFFFF, r_info >> 32, r_add))
        # DT_RELR (packed relative relocations, newer SDKs): an even entry is the offset of a word to relocate, an
        # odd one a bitmap of the 63 words after the last one; the addend is the word itself (runtime/loader.c)
        if DT_RELR in dyn:
            where = None
            for i in range(dyn.get(DT_RELRSZ, 0) // 8):
                e = self.u64(dyn[DT_RELR] + 8 * i)
                if not e & 1:
                    self.relocs.append((e, R_AARCH64_RELATIVE, 0, self.u64(e)))
                    where = e + 8
                    continue
                for b in range(63):
                    if (e >> (b + 1)) & 1:
                        self.relocs.append((where + 8 * b, R_AARCH64_RELATIVE, 0, self.u64(where + 8 * b)))
                where += 63 * 8

    def _nsyms(self):
        """Number of dynsym entries: DT_HASH's nchain, else the end of DT_GNU_HASH's last chain (newer SDK modules,
        e.g. sdk and subsdk0 of BOTW 1.8.2, have only the GNU table)."""
        if DT_HASH in self.dyn:
            return self.u32(self.dyn[DT_HASH] + 4)
        if DT_GNU_HASH not in self.dyn:
            return 0
        h = self.dyn[DT_GNU_HASH]
        nbuckets, symoffset, bloom_size = self.u32(h), self.u32(h + 4), self.u32(h + 8)
        buckets = h + 16 + 8 * bloom_size
        last = max((self.u32(buckets + 4 * i) for i in range(nbuckets)), default=0)
        if last < symoffset:  # (no hashed symbols)
            return symoffset
        chain = buckets + 4 * nbuckets
        while not self.u32(chain + 4 * (last - symoffset)) & 1:
            last += 1
        return last + 1

    def cstr(self, off):
        end = self.image.index(b'\0', off)
        return self.image[off:end].decode('utf-8', 'replace')

    def imports(self):
        return [s for s in self.syms if s[0] and s[2] == 0]

    def exports(self):
        return [s for s in self.syms if s[0] and s[2] != 0]


if __name__ == '__main__':
    from collections import Counter
    for p in sys.argv[1:]:
        n = Nso(p)
        print(f'== {p}  build {n.build_id[:16]}  image 0x{n.size:x}')
        print(f'  text 0x{n.text[0]:x}+0x{n.text[1]:x}  ro 0x{n.rodata[0]:x}+0x{n.rodata[1]:x}  '
              f'data 0x{n.data[0]:x}+0x{n.data[1]:x}  bss 0x{n.bss_size:x}')
        print(f'  needed {n.needed}  syms {len(n.syms)}  imports {len(n.imports())}  exports {len(n.exports())}')
        print(f'  relocs {dict(Counter(r[1] for r in n.relocs))}')
