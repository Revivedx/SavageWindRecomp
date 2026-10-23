"""Module layout test with synthetic NSOs (made-up modules, no game code): layout.py, gen.py and sw_probe.py agree.

Builds an exefs whose modules are sized like a big game's (a main larger than the 64 MB LASRecomp left before sdk,
two subsdk, an odd-sized sdk, an rtld to be skipped), each a few hand-assembled functions plus zero-filled segments;
subsdk0 and sdk list their symbols with DT_GNU_HASH only and keep their relative relocations in DT_RELR only, as
BOTW 1.8.2's do. Then it checks:
- nso.py finds every module's symbols and relative relocations, whichever tables they are in;
- the layout: load order, back to back from MAIN_BASE, page-rounded images, inside the module area; and that a set
  of modules too big for the area, or one without sdk, is refused;
- `gen.py all`: layout.txt / layout.c match, every generated function is where layout + symbol says, a module gone
  from the exefs loses its old folder; `gen.py <module>` with that layout.txt writes the same table, and refuses a
  file other than the one the layout was made from;
- sw_probe.py reports the same addresses;
- jump tables (jump_table_module(): no layout involved): gen.py keeps a function with tables whole (an adr of a table
  base inside it is a label, not a new function), finds each table's cases wherever the base and the cases are in the
  function's eh_frame range, and when the function is split anyway reaches the cases in another piece by tail calls.

--keep DIR leaves DIR/exefs and DIR/gen behind: `make GEN=DIR/gen` builds a game code library from them, and the
runtime started on DIR (`<exe> --play DIR`) loads the modules where that library's layout says, then stops at the
missing nn::init::Start.

Usage: test_layout.py [--keep DIR]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import lz4.block

import game_symbols
import layout
from nso import Nso

TOOLS = os.path.dirname(os.path.abspath(__file__))
MOD0_AT = 0x8      # MOD0 header inside text (word 1 of text points at it)
FUNCS_AT = 0x40    # first function
MOVZ_X0, RET, BL = 0xD2800000, 0xD65F03C0, 0x94000000
OLD_SDK_BASE = 0x7004000000  # where LASRecomp's fixed layout put sdk
# like BOTW 1.8.2's: symbols in DT_GNU_HASH only (no DT_HASH), relative relocations in DT_RELR only
NEW_SDK = {'subsdk0', 'sdk'}

# name: (rodata, data, bss sizes, exported functions, imports, build id: 8 bytes as hex)
# main's is BOTW 1.8.2's: gen.py takes the patch sites from tools/patches.py and the runtime's addresses from
# tools/game_symbols.py by it.
MODULES = {
    'rtld': (0x1000, 0x1000, 0, 2, [], '1010101010101010'),
    'main': (0x2F00000, 0x400000, 0x1A00123, 6, ['sw_test_subsdk0_fn1'], 'de0b6ac4efc1dda7'),
    'subsdk0': (0x600000, 0x100000, 0x80000, 4, [], '1212121212121212'),
    'subsdk1': (0x1000, 0x1000, 0x123, 2, ['sw_test_sdk_fn0'], '1313131313131313'),
    'sdk': (0x800000, 0x200000, 0x300321, 5, [], '1414141414141414'),
}


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def nso_file(segs, bss, build_id):
    """NSO0 container (the header nso.py and runtime/loader.c read), every segment LZ4-compressed."""
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    struct.pack_into('<I', hdr, 0xC, 7)
    body, file_off, mem = b'', 0x100, 0
    for i, seg in enumerate(segs):
        packed = lz4.block.compress(bytes(seg), store_size=False)
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, file_off, mem, len(seg))
        struct.pack_into('<I', hdr, 0x60 + i * 4, len(packed))
        body += packed
        file_off += len(packed)
        mem = align(mem + len(seg), 0x1000)
    struct.pack_into('<I', hdr, 0x3C, bss)
    hdr[0x40:0x60] = bytes.fromhex(build_id * 4)
    return bytes(hdr) + body


def gnu_hash(name):
    h = 5381
    for c in name.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def module(name, ro_size, data_size, bss, nfn, imports, build_id):
    """A synthetic module: (file bytes, {exported name: text offset}, [(relative relocation offset, target)])."""
    tag = name
    fns = {f'sw_test_{tag}_fn{k}': FUNCS_AT + 8 * k for k in range(nfn)}
    text = bytearray(FUNCS_AT + 8 * nfn)
    struct.pack_into('<II', text, 0, 0x14000002, MOD0_AT)  # b +8; MOD0 offset
    for k in range(nfn):
        first = MOVZ_X0 | (k << 5)
        if k == nfn - 1 and nfn > 1:  # the last one calls the first: a BL inside the module
            first = BL | (((FUNCS_AT - (FUNCS_AT + 8 * k)) // 4) & 0x3FFFFFF)
        struct.pack_into('<II', text, FUNCS_AT + 8 * k, first, RET)
    ro_off = align(len(text), 0x1000)
    data_off = ro_off + align(ro_size, 0x1000)

    # rodata: dynstr, dynsym, hash, rela; data: dynamic, a pointer (R_RELATIVE) and a slot per import (GLOB_DAT).
    # Imports first: a GNU hash table only covers the symbols after them.
    syms = [(0, 0, 0, 0)] + [(n, 0x12, 0, 0) for n in imports] + [(n, 0x12, 1, off) for n, off in fns.items()]
    strtab = bytearray(b'\0')
    names = []
    for n, *_ in syms[1:]:
        names.append(len(strtab))
        strtab += n.encode() + b'\0'
    ro = bytearray(ro_size)
    o = 0
    strtab_at = ro_off + o
    ro[o:o + len(strtab)] = strtab
    o = align(o + len(strtab), 8)
    symtab_at = ro_off + o
    for i, (n, info, shndx, value) in enumerate(syms):
        struct.pack_into('<IBBHQQ', ro, o + 24 * i, names[i - 1] if i else 0, info, 0, shndx, value, 8 if value else 0)
    o += 24 * len(syms)
    hash_at = ro_off + o
    if name in NEW_SDK:  # one bucket, a bloom filter that passes everything
        first = 1 + len(imports)
        hashes = [gnu_hash(n) for n, *_ in syms[first:]]
        chain = [(h & ~1) | (i == len(hashes) - 1) for i, h in enumerate(hashes)]
        struct.pack_into(f'<IIIIQI{len(chain)}I', ro, o, 1, first, 1, 6, (1 << 64) - 1, first if chain else 0, *chain)
        o += 24 + 4 + 4 * len(chain)
        hash_tag = 0x6FFFFEF5
    else:
        chain = [0] + [i + 1 if i + 1 < len(syms) else 0 for i in range(1, len(syms))]
        struct.pack_into(f'<II I{len(syms)}I', ro, o, 1, len(syms), 1 if len(syms) > 1 else 0, *chain)
        o += 12 + 4 * len(syms)
        hash_tag = 4
    o = align(o, 8)
    rela_at = ro_off + o
    dyn_size = 16 * 14
    ptr_slot = data_off + dyn_size
    # pointers to functions: the first one at ptr_slot, the last one past the import slots
    relative = [(ptr_slot, FUNCS_AT), (ptr_slot + 8 * (len(imports) + 1), FUNCS_AT + 8 * (nfn - 1))]
    relas = [] if name in NEW_SDK else [(off, 1027, 0, to) for off, to in relative]  # R_AARCH64_RELATIVE
    for j, n in enumerate(imports):
        relas.append((ptr_slot + 8 * (j + 1), 1025, 1 + j, 0))  # R_AARCH64_GLOB_DAT
    for i, (off, typ, sym, add) in enumerate(relas):
        struct.pack_into('<QQq', ro, o + 24 * i, off, (sym << 32) | typ, add)
    o += 24 * len(relas)
    relr_at = ro_off + o  # DT_RELR: the first pointer's offset, then a bitmap (bit k: the word 8 * k after it)
    relr = [relative[0][0], 1 | (1 << (len(imports) + 1))] if name in NEW_SDK else []
    struct.pack_into(f'<{len(relr)}Q', ro, o, *relr)
    o += 8 * len(relr)
    assert o <= ro_size, name

    data = bytearray(data_size)
    if name in NEW_SDK:  # (DT_RELR: the addend is the word itself)
        for off, to in relative:
            struct.pack_into('<Q', data, off - data_off, to)
    dyn = [(hash_tag, hash_at), (5, strtab_at), (6, symtab_at), (10, len(strtab)), (11, 24), (7, rela_at),
           (8, 24 * len(relas)), (9, 24)]
    if relr:
        dyn += [(35, 8 * len(relr)), (36, relr_at), (37, 8)]  # DT_RELRSZ, DT_RELR, DT_RELRENT
    dyn.append((0, 0))
    assert 16 * len(dyn) <= dyn_size
    for i, (t, v) in enumerate(dyn):
        struct.pack_into('<QQ', data, 16 * i, t, v)
    bss_at = data_off + data_size
    mod0 = struct.pack('<4s6i', b'MOD0', data_off - MOD0_AT, bss_at - MOD0_AT, bss_at + bss - MOD0_AT,
                       -MOD0_AT, -MOD0_AT, bss_at - MOD0_AT)  # (no .eh_frame_hdr: offsets to 0)
    text[MOD0_AT:MOD0_AT + len(mod0)] = mod0
    return nso_file([text, ro, data], bss, build_id), fns, relative


# ---------------------------------------------------------------- jump tables

# hand-assembled AArch64, for the jump table module (offsets: from the text start; the module base is page-aligned)
def b_(pc, to, op=0x14000000): return op | (((to - pc) >> 2) & 0x3FFFFFF)
def b_hi(pc, to): return 0x54000008 | ((((to - pc) >> 2) & 0x7FFFF) << 5)
def cbz(rt, pc, to): return 0x34000000 | ((((to - pc) >> 2) & 0x7FFFF) << 5) | rt
def tbnz(rt, bit, pc, to): return 0x37000000 | (bit << 19) | ((((to - pc) >> 2) & 0x3FFF) << 5) | rt
def adr(rd, pc, to): return 0x10000000 | (((to - pc) & 3) << 29) | ((((to - pc) >> 2) & 0x7FFFF) << 5) | rd
def adrp(rd, pc, to):
    d = (to >> 12) - (pc >> 12)
    return 0x90000000 | ((d & 3) << 29) | (((d >> 2) & 0x7FFFF) << 5) | rd
def add_lo12(rd, rn, to): return 0x91000000 | ((to & 0xFFF) << 10) | (rn << 5) | rd  # add xd, xn, #:lo12:to
def cmp_w(rn, imm): return 0x7100001F | (imm << 10) | (rn << 5)
def mov_w(rd, rm): return 0x2A0003E0 | (rm << 16) | rd
def ldrb_x(rt, rn, rm): return 0x38606800 | (rm << 16) | (rn << 5) | rt  # ldrb wt, [xn, xm]
def ldrh_x(rt, rn, rm): return 0x78607800 | (rm << 16) | (rn << 5) | rt  # ldrh wt, [xn, xm, lsl #1]
def add_lsl2(rd, rn, rm): return 0x8B000800 | (rm << 16) | (rn << 5) | rd  # add xd, xn, xm, lsl #2
def br(rn): return 0xD61F0000 | (rn << 5)
def movz_x0(v): return MOVZ_X0 | (v << 5)


# Three functions, each with an FDE. F is printf_core's shape (musl, behind vfprintf in BOTW 1.8.2's sdk): a byte
# table whose adr base is the first case right after the br, its index bounds-checked by a cmp with a tbnz in between
# and its base loaded before the loop; then a 16-bit table whose base is the loop head, before the br. G has a byte
# table too, but H calls into G's middle, so G must be split there: the case that falls in the other piece is entered
# by a tail call.
JT_CODE = [
    'F', lambda pc, L: adrp(11, pc, L['TBL1']), lambda pc, L: add_lo12(11, 11, L['TBL1']),
    'F_LOOP', lambda pc, L: cbz(0, pc, L['F_EXIT']), mov_w(8, 1), cmp_w(8, 3),
    lambda pc, L: tbnz(2, 6, pc, L['F_EXIT']), lambda pc, L: b_hi(pc, L['F_DEF']),
    lambda pc, L: adr(9, pc, L['F_C1']), ldrb_x(10, 11, 8), add_lsl2(9, 9, 10), 'F_BR1', br(9),
    'F_C1', movz_x0(1), lambda pc, L: b_(pc, L['F_LOOP']), movz_x0(2), lambda pc, L: b_(pc, L['F_LOOP']),
    movz_x0(3), lambda pc, L: b_(pc, L['F_T2']),
    'F_DEF', movz_x0(4),
    'F_T2', mov_w(8, 3), cmp_w(8, 2), lambda pc, L: b_hi(pc, L['F_LOOP']),
    lambda pc, L: adrp(12, pc, L['TBL2']), lambda pc, L: add_lo12(12, 12, L['TBL2']),
    lambda pc, L: adr(9, pc, L['F_LOOP']), ldrh_x(10, 12, 8), add_lsl2(9, 9, 10), 'F_BR2', br(9),
    'F_C2', movz_x0(5),
    'F_EXIT', RET, 'F_END',
    'G', lambda pc, L: adrp(11, pc, L['TBL3']), lambda pc, L: add_lo12(11, 11, L['TBL3']),
    'G_LOOP', lambda pc, L: cbz(0, pc, L['G_EXIT']),
    'G_MID', mov_w(8, 1), cmp_w(8, 1), lambda pc, L: b_hi(pc, L['G_EXIT']),
    lambda pc, L: adr(9, pc, L['G_LOOP']), ldrb_x(10, 11, 8), add_lsl2(9, 9, 10), 'G_BR', br(9),
    'G_C1', movz_x0(6),
    'G_EXIT', RET, 'G_END',
    'H', lambda pc, L: b_(pc, L['G_MID'], BL), RET, 'H_END',
]
# the cases of each table (labels, + 4 * instruction): what gen.py must find
JT_CASES = {
    'F_BR1': [('F_C1', 0), ('F_C1', 2), ('F_C1', 4), ('F_DEF', 0)],
    'F_BR2': [('F_LOOP', 0), ('F_C2', 0), ('F_EXIT', 0)],
    'G_BR': [('G_LOOP', 0), ('G_C1', 0)],
}
JT_TABLES = {'TBL1': ('B', 'F_BR1', 'F_C1'), 'TBL2': ('H', 'F_BR2', 'F_LOOP'), 'TBL3': ('B', 'G_BR', 'G_LOOP')}


def eh_frame(at, funcs):
    """.eh_frame_hdr and then .eh_frame for module offset `at`: one CIE (zR, pc-relative sdata4) and an FDE (no CFI
    instructions) per (start, end) in funcs."""
    hdr = struct.pack('<4B', 1, 0x1B, 0x03, 0x3B)  # version, eh_frame_ptr pcrel|sdata4, count udata4, table datarel|sdata4
    frame = at + 12 + 8 * len(funcs)
    hdr += struct.pack('<iI', frame - (at + 4), len(funcs))
    # CIE: id 0, version 1, "zR", code align 1, data align -4, return address x30, augmentation: the FDE encoding
    body = struct.pack('<IB3sBBBBB', 0, 1, b'zR\0', 1, 0x7C, 30, 1, 0x1B) + b'\0' * 3
    out = struct.pack('<I', len(body)) + body
    table = b''
    for start, end in sorted(funcs):
        fde = frame + len(out)
        table += struct.pack('<ii', start - at, fde - at)
        body = struct.pack('<iiiB', fde + 4 - frame, start - (fde + 8), end - start, 0) + b'\0' * 3
        out += struct.pack('<I', len(body)) + body
    return hdr + table + out + bytes(4)  # (a zero length ends .eh_frame)


def jump_table_module():
    """A module with JT_CODE in its text, the tables in rodata, an eh_frame for F, G and H, and nothing else:
    (file bytes, {label: module offset})."""
    L, pc = {}, FUNCS_AT
    for item in JT_CODE:  # (labels first: the code refers forward)
        if isinstance(item, str):
            L[item] = pc
        else:
            pc += 4
    ro_off = align(pc, 0x1000)
    for k, name in enumerate(JT_TABLES):
        L[name] = ro_off + 0x10 * k
    text = bytearray(pc)
    struct.pack_into('<II', text, 0, 0x14000002, MOD0_AT)
    pc = FUNCS_AT
    for item in JT_CODE:
        if not isinstance(item, str):
            struct.pack_into('<I', text, pc, item(pc, L) if callable(item) else item)
            pc += 4
    ro = bytearray(0x1000)
    for name, (fmt, br_label, base) in JT_TABLES.items():
        entries = [(L[lab] + 4 * k - L[base]) // 4 for lab, k in JT_CASES[br_label]]
        struct.pack_into(f'<{len(entries)}{fmt}', ro, L[name] - ro_off, *entries)
    eh_at = ro_off + 0x100
    eh = eh_frame(eh_at, [(L[f], L[f + '_END']) for f in 'FGH'])
    ro[0x100:0x100 + len(eh)] = eh
    data_off = ro_off + len(ro)
    data = bytearray(0x1000)  # .dynamic: just DT_NULL
    mod0 = struct.pack('<4s6i', b'MOD0', data_off - MOD0_AT, data_off + len(data) - MOD0_AT,
                       data_off + len(data) - MOD0_AT, eh_at - MOD0_AT, eh_at + len(eh) - MOD0_AT,
                       data_off + len(data) - MOD0_AT)
    text[MOD0_AT:MOD0_AT + len(mod0)] = mod0
    return nso_file([text, ro, data], 0, '3030303030303030'), L


def jump_tables(root, check):
    """gen.py on jump_table_module(): function ranges from the eh_frame, the tables' cases, and the C."""
    import gen  # (Unicorn: gen.py emulates the address math of each table)
    path = os.path.join(root, 'jumptables')
    raw, off = jump_table_module()
    with open(path, 'wb') as f:
        f.write(raw)
    base = layout.MODULES_LO
    L = {k: base + v for k, v in off.items()}
    m = gen.Module('sdk', path, base, 'c')
    check(m.fdes == [(L[f], L[f + '_END']) for f in 'FGH'], f'eh_frame function ranges ({m.fdes})')
    entries, rounds = gen.discover(m, m.boundaries())
    check(sorted(entries) == sorted(L[k] for k in ('F', 'G', 'G_LOOP', 'G_MID', 'G_EXIT', 'H')),
          f'F stays one function, the adr bases inside it are labels; G is split at the call, then at its table\'s '
          f'case and its exit ({[hex(a) for a in sorted(entries)]})')
    cases = {br: [L[lab] + 4 * k for lab, k in want] for br, want in JT_CASES.items()}
    check(entries[L['F']][2] == {L['F_BR1']: sorted(cases['F_BR1']), L['F_BR2']: sorted(cases['F_BR2'])},
          f'F: both tables resolved ({entries[L["F"]][2]})')
    check(entries[L['G_MID']][2] == {L['G_BR']: sorted(cases['G_BR'])}, 'G: its table resolved from the split piece')
    check(+m.stats == {'jumptables': 3}, f'indirect branch count ({dict(m.stats)})')
    c = '\n'.join(m.emit_function(L['F'], *entries[L['F']], {}))
    check(all(f'case {t:#x}ull: goto L_{t:x};' in c for t in cases['F_BR1'] + cases['F_BR2']), 'F: cases are gotos')
    c = '\n'.join(m.emit_function(L['G_MID'], *entries[L['G_MID']], {}))
    check(f'case {L["G_LOOP"]:#x}ull: __attribute__((musttail)) return f_{L["G_LOOP"]:x}(c);' in c and
          f'case {L["G_C1"]:#x}ull: goto L_{L["G_C1"]:x};' in c, 'G: the case in the other piece is a tail call')


def make_exefs(d, modules=MODULES):
    os.makedirs(d, exist_ok=True)
    fns, relative = {}, {}
    for name, spec in modules.items():
        raw, fns[name], relative[name] = module(name, *spec)
        with open(os.path.join(d, name), 'wb') as f:
            f.write(raw)
    with open(os.path.join(d, 'main.npdm'), 'wb') as f:
        f.write(b'META' + bytes(0x7C))
    return fns, relative


def table(path):
    """Function addresses of a generated <module>_table.c."""
    src = open(path).read()
    body = src[src.index('_fn_addrs[] = {') + 15:src.index('};', src.index('_fn_addrs[]'))]
    count = int(re.search(r'_fn_count = (\d+);', src).group(1))
    addrs = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body)]
    return addrs[:count]


def run(*args, ok=True):
    r = subprocess.run([sys.executable, *args], cwd=TOOLS, capture_output=True, text=True)
    if (r.returncode == 0) != ok:
        sys.exit(f'FAIL: {" ".join(args)} exited {r.returncode}\n{r.stdout}{r.stderr}')
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--keep')
    args = ap.parse_args()
    root = os.path.abspath(args.keep) if args.keep else tempfile.mkdtemp(prefix='sw_layout_')
    exefs, gen = os.path.join(root, 'exefs'), os.path.join(root, 'gen')
    shutil.rmtree(root, ignore_errors=True)
    fns, relative = make_exefs(exefs)
    checks = 0

    def check(cond, what):
        nonlocal checks
        if not cond:
            sys.exit(f'FAIL: {what}')
        checks += 1

    # ---- layout.py
    lay = layout.compute(exefs)
    layout.show(lay)
    check([m[0] for m in lay] == ['main', 'subsdk0', 'subsdk1', 'sdk'], 'load order (rtld left out)')
    check(lay[0][1] == layout.MODULES_LO, 'main at MAIN_BASE')
    for (n, b, s, i), nxt in zip(lay, lay[1:] + [None]):
        ro, data, bss = MODULES[n][:3]
        text = FUNCS_AT + 8 * MODULES[n][3]
        extent = align(text, 0x1000) + align(ro, 0x1000) + data + bss
        check(s == align(extent, layout.PAGE) and b % layout.PAGE == 0, f'{n}: image size {s:#x}')
        check(nxt is None or nxt[1] == b + s, f'{n}: next module right after it')
        check(i == MODULES[n][5] * 4, f'{n}: build id')
    check(lay[0][1] + lay[0][2] > OLD_SDK_BASE, 'main is past where LASRecomp put sdk (the case this is for)')
    check(layout.problems(lay) == [], 'loadable')
    for n, spec in MODULES.items():
        nso = Nso(os.path.join(exefs, n))
        check(sorted(s[0] for s in nso.exports()) == sorted(fns[n]) and
              sorted(s[0] for s in nso.imports()) == sorted(spec[4]),
              f'{n}: symbols found through {"DT_GNU_HASH" if n in NEW_SDK else "DT_HASH"}')
        check(sorted((r[0], r[3]) for r in nso.relocs if r[1] == 1027) == sorted(relative[n]),
              f'{n}: relative relocations found in {"DT_RELR" if n in NEW_SDK else "DT_RELA"}')

    with tempfile.TemporaryDirectory() as t:  # too big for the module area / no sdk: refused
        big = dict(MODULES, main=(0x1000, 0x1000, 0x7FF00000, 1, [], '2121212121212121'))  # (+ sdk: past 2 GiB)
        make_exefs(t, {k: v for k, v in big.items() if k in ('main', 'sdk')})
        why = layout.problems(layout.compute(t))
        check(len(why) == 1 and 'past' in why[0], f'oversized layout refused: {why}')
        os.remove(os.path.join(t, 'sdk'))
        check(any('no sdk' in w for w in layout.problems(layout.compute(t))), 'layout without sdk refused')
        r = run('gen.py', 'layout', t, os.path.join(t, 'out'), ok=False)
        check('no sdk' in r.stderr, 'gen.py layout refuses it too')

    # ---- gen.py all
    os.makedirs(os.path.join(gen, 'subsdk5'))  # (an earlier exefs's module: must go)
    run('gen.py', 'all', exefs, gen, '--simd', 'c')
    check(not os.path.exists(os.path.join(gen, 'subsdk5')), 'stale module folder removed')
    check(layout.read(os.path.join(gen, 'layout.txt')) == lay, 'layout.txt == layout.compute()')
    c = open(os.path.join(gen, 'layout.c')).read()
    for n, b, s, i in lay:
        check(f'{{"{n}", {b:#x}ull, {s:#x}ull, "{i}", {n}_fn_addrs, {n}_fn_ptrs, &{n}_fn_count}}' in c,
              f'layout.c entry for {n}')
        got = table(os.path.join(gen, n, f'{n}_table.c'))
        want = sorted(b + off for off in fns[n].values())
        check(got == want, f'{n}: generated functions at base + symbol ({[hex(a) for a in got]})')
        chunk = open(os.path.join(gen, n, f'{n}_000.c')).read()
        check(all(f'void f_{a:x}(Cpu *c)' in chunk for a in want), f'{n}: functions named by address')
    check(f'game_module_count = {len(lay)};' in c, 'layout.c module count')
    syms = game_symbols.symbols('de0b6ac4efc1dda7')  # (main's build id: MODULES)
    check(all(f'{{"{n}", {v:#x}ull}},' in c for n, (kind, v) in syms), 'layout.c: the game addresses')
    check(f'game_symbol_count = {len(syms)};' in c, 'layout.c symbol count')

    # ---- gen.py <module> with layout.txt
    one = os.path.join(root, 'one')
    run('gen.py', 'subsdk0', os.path.join(exefs, 'subsdk0'), os.path.join(gen, 'layout.txt'), one, '--simd', 'c')
    check(open(os.path.join(one, 'subsdk0_table.c')).read() == open(os.path.join(gen, 'subsdk0', 'subsdk0_table.c')).read(),
          'per-module run == all')
    r = run('gen.py', 'sdk', os.path.join(exefs, 'main'), os.path.join(gen, 'layout.txt'), one, ok=False)
    check('not the sdk' in r.stderr, 'another file than the layout was made from: refused')
    shutil.rmtree(one)

    # ---- sw_probe.py
    rep = os.path.join(root, 'probe.json')
    run('sw_probe.py', exefs, '--sample', '1', '--json', rep)
    import json
    probe = json.load(open(rep))
    check([(m['module'], int(m['base'], 16), int(m['end'], 16)) for m in probe['layout']] ==
          [(n, b, b + s) for n, b, s, i in lay] and probe['layout_problems'] == [], 'sw_probe layout')
    check(all(int(probe['modules'][n]['base'], 16) == b for n, b, s, i in lay), 'sw_probe probes at those bases')

    # ---- jump tables
    jump_tables(root, check)

    print(f'ok: {checks} checks' + (f', files in {root}' if args.keep else ''))
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == '__main__':
    main()
