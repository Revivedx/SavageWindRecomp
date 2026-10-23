"""Function discovery and instruction census for an NSO module."""
import struct
import sys
from collections import Counter

import capstone
from nso import Nso, R_AARCH64_RELATIVE


def read_encoded(img, off, enc, pc_base):
    fmt = enc & 0x0F
    app = enc & 0x70
    if fmt == 0x0B:      # sdata4
        v, size = struct.unpack_from('<i', img, off)[0], 4
    elif fmt == 0x03:    # udata4
        v, size = struct.unpack_from('<I', img, off)[0], 4
    elif fmt in (0x0C, 0x04):
        v, size = struct.unpack_from('<q' if fmt == 0x0C else '<Q', img, off)[0], 8
    else:
        raise ValueError(f'unsupported encoding {enc:#x}')
    if app == 0x10:
        v += off
    elif app == 0x30:
        v += pc_base
    return v, size


def eh_frame_functions(n):
    """Function start addresses from .eh_frame_hdr's binary search table."""
    img, hdr = n.image, n.eh_start
    if not hdr or hdr >= len(img):
        return []
    ver, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
    off = hdr + 4
    _, sz = read_encoded(img, off, ptr_enc, hdr)
    off += sz
    count, sz = read_encoded(img, off, cnt_enc, hdr)
    off += sz
    starts = []
    for _ in range(count):
        loc, sz = read_encoded(img, off, tbl_enc, hdr)
        off += sz
        _, sz = read_encoded(img, off, tbl_enc, hdr)
        off += sz
        starts.append(loc)
    return starts


def uleb128(img, off):
    v = shift = 0
    while True:
        b = img[off]
        off += 1
        v |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return v, off


def eh_frame_extents(n):
    """[start, end) of every function with an FDE: the address range each one covers. [] if .eh_frame
    isn't readable."""
    img, hdr = n.image, n.eh_start
    if not hdr or hdr >= len(img):
        return []
    try:
        ver, ptr_enc, cnt_enc, tbl_enc = img[hdr:hdr + 4]
        off = hdr + 4
        _, sz = read_encoded(img, off, ptr_enc, hdr)
        off += sz
        count, sz = read_encoded(img, off, cnt_enc, hdr)
        off += sz
        fde_enc = {}  # CIE -> pointer encoding of its FDEs ('R' augmentation)
        out = []
        for _ in range(count):
            loc, sz = read_encoded(img, off, tbl_enc, hdr)
            off += sz
            fde, sz = read_encoded(img, off, tbl_enc, hdr)
            off += sz
            cie = fde + 4 - struct.unpack_from('<I', img, fde + 4)[0]
            if cie not in fde_enc:
                o = cie + 8
                version = img[o]
                aug_end = img.index(b'\0', o + 1)
                aug = img[o + 1:aug_end].decode()
                _, o = uleb128(img, aug_end + 1)  # code alignment
                _, o = uleb128(img, o)  # data alignment (sleb128: same length)
                o = o + 1 if version == 1 else uleb128(img, o)[1]  # return address register
                enc = 0
                if aug.startswith('z'):
                    _, o = uleb128(img, o)
                    for ch in aug[1:]:
                        if ch == 'R':
                            enc = img[o]
                            o += 1
                        elif ch == 'P':
                            o += 1 + read_encoded(img, o + 1, img[o], 0)[1]
                        elif ch == 'L':
                            o += 1
                fde_enc[cie] = enc
            enc = fde_enc[cie]
            pc, sz = read_encoded(img, fde + 8, enc, 0)
            size, _ = read_encoded(img, fde + 8 + sz, enc & 0x0F, 0)  # (the range: same format, absolute)
            if pc == loc:
                out.append((loc, loc + size))
        return out
    except (ValueError, IndexError, struct.error):
        return []


def discover(n):
    text_lo, text_hi = n.text[0], n.text[0] + n.text[1]
    in_text = lambda a: text_lo <= a < text_hi and a % 4 == 0
    funcs = {}
    for a in eh_frame_functions(n):
        if in_text(a):
            funcs[a] = 'eh'
    for name, info, shndx, value, size in n.syms:
        if shndx and in_text(value) and (info & 0xF) == 2:
            funcs.setdefault(value, 'sym')
    # Code pointers stored in data (vtables, callbacks)
    ptr_targets = set()
    for off, typ, sym, add in n.relocs:
        if typ == R_AARCH64_RELATIVE and in_text(add):
            ptr_targets.add(add)
    for a in ptr_targets:
        funcs.setdefault(a, 'ptr')
    # BL targets
    code = bytes(n.image[text_lo:text_hi])
    words = struct.unpack(f'<{len(code) // 4}I', code)
    for i, w in enumerate(words):
        if (w & 0xFC000000) == 0x94000000:
            imm = w & 0x03FFFFFF
            if imm & 0x02000000:
                imm -= 0x04000000
            t = text_lo + i * 4 + imm * 4
            if in_text(t):
                funcs.setdefault(t, 'bl')
    return funcs, words


def census(n, words):
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    c = Counter()
    bad = 0
    code = struct.pack(f'<{len(words)}I', *words)
    off = 0
    while off < len(code):
        got = False
        for ins in md.disasm_lite(code[off:], n.text[0] + off):
            c[ins[2]] += 1
            off += 4
            got = True
        if off < len(code):   # undecodable word: skip it
            bad += 1
            off += 4
    return c, bad


if __name__ == '__main__':
    n = Nso(sys.argv[1])
    funcs, words = discover(n)
    print(f'functions: {len(funcs)}  by source: {dict(Counter(funcs.values()))}')
    if '--census' in sys.argv:
        c, bad = census(n, words)
        print(f'instructions: {sum(c.values())}  undecodable words: {bad}  distinct mnemonics: {len(c)}')
        print(' '.join(f'{m}:{k}' for m, k in c.most_common()))
