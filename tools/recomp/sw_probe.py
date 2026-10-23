"""Savage Wind probe: how far the generator gets on a game's exefs, without writing any C.

Usage: sw_probe.py [exefs_dir] [--modules main,sdk] [--sample N] [--json report.json]

It prints the guest addresses `make gen` gives the modules (tools/recomp/layout.py) and whether the runtime can load them
there. For each module (default: all of them) it reports the NSO layout, the functions gen.py finds, which
instructions the lifter cannot translate yet (grouped by reason and by Capstone mnemonic) and an estimate of the C
that `make gen` would write.
It also lists the NVN entry points the game names in its own data next to the ones runtime/nvn
implements, which is the first view of the renderer work.

Nothing is written next to the game files; the optional JSON report goes where --json says.
"""
import argparse
import glob
import json
import os
import platform
import re
import sys
import time
from collections import Counter

import capstone

import layout
from gen import Module, discover
from lifter import Unsupported
from nso import Nso

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(TOOLS))


def nvn_implemented():
    """NVN entry points the runtime binds to host code, and the ones bound to an empty body: NVN(nvnX) in
    runtime/nvn, HLE_FUNC("nvnX", ...) in runtime/hle (the bootstrap loader and GetProcAddress)."""
    names, stubs = set(), set()
    paths = glob.glob(os.path.join(ROOT, 'runtime', 'nvn', '*.c')) + glob.glob(os.path.join(ROOT, 'runtime', 'hle', '*.c'))
    for path in paths:
        for line in open(path, encoding='utf-8', errors='replace'):
            mm = re.match(r'\s*[A-Z_]+\("?(nvn[A-Z]\w*)', line)
            if mm:
                names.add(mm.group(1))
                if re.search(r'\{\s*\(void\)c;\s*\}', line):
                    stubs.add(mm.group(1))
    return names, stubs


def nvn_requested(nso):
    """NVN function names stored as strings in a module (the game looks them up by name)."""
    return {s.decode() for s in re.findall(rb'(?<![\w])(nvn[A-Z][A-Za-z0-9]{3,})\0', bytes(nso.image))}


def import_groups(nso):
    """Imported symbols grouped by their leading C++ namespaces (nn::fs, nn::os, std::__1, ...); C symbols by their
    name up to the first '_' after the leading ones (__cxa, glslcCompile, sinf)."""
    groups = Counter()
    for name, *_ in nso.imports():
        # _Z, or _ZTV/_ZTI/_ZTS (vtable, typeinfo), then N + cv/ref qualifiers for a nested name
        mm = re.match(r'_Z(?:T[VIS])?(N[rVKRO]*)?(?=St|\d)', name)
        if name.startswith('_Z') and not mm:  # (_Znwm, _ZdlPv...: operator new / delete)
            groups['(C++) operator'] += 1
            continue
        if not mm:
            groups[name if name.startswith('nvn') else '(C) ' + re.match(r'_*[^_]*', name).group()] += 1
            continue
        parts, rest = [], name[mm.end():]
        while len(parts) < (2 if mm.group(1) else 1):  # (not nested: only the name itself)
            if rest.startswith('St'):
                parts.append('std')
                rest = rest[2:]
                continue
            m2 = re.match(r'\d+', rest)
            if not m2:
                break
            n = int(m2.group())
            rest = rest[m2.end():]
            parts.append(rest[:n])
            rest = rest[n:]
        groups['::'.join(parts)] += 1
    return groups


def probe_module(name, path, base, simd, sample):
    t0 = time.time()
    rep = {'name': name, 'path': path, 'base': hex(base)}
    n = Nso(path)
    rep['build_id'] = n.build_id
    rep['layout'] = {'text': n.text[1], 'rodata': n.rodata[1], 'data': n.data[1], 'bss': n.bss_size,
                     'image': n.size}
    rep['needed'] = n.needed
    rep['imports'] = len(n.imports())
    rep['exports'] = len(n.exports())
    rep['import_groups'] = dict(import_groups(n).most_common(25))

    m = Module(name, path, base, simd)
    starts = m.boundaries()
    entries, rounds = discover(m, starts)
    rep['discovery'] = {'boundaries': len(starts), 'functions': len(entries), 'rounds': rounds,
                        'stats': dict(m.stats), 'seconds': round(time.time() - t0)}

    # Lift every instruction inside a discovered function and keep the reasons for the ones the
    # lifter rejects. Data inside text (literal pools, padding) also lands here as "invalid".
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    kinds, reasons, mnems = Counter(), Counter(), Counter()
    examples = {}
    total = 0
    for a, (end, _labels, _tables) in entries.items():
        for pc in range(a, end, 4):
            total += 1
            w = m.word(pc)
            try:
                kinds[m.lifter.lift(w, pc).kind] += 1
            except Unsupported as e:
                reason = re.sub(r'[0-9a-f]{8}', 'XXXXXXXX', str(e))
                reasons[reason] += 1
                d = next(cs.disasm(w.to_bytes(4, 'little'), pc), None)
                mn = d.mnemonic if d else '(invalid / data)'
                mnems[mn] += 1
                examples.setdefault(mn, f'{pc:#x}: {w:08x} {d.mnemonic + " " + d.op_str if d else ""}')
    unsupported = sum(reasons.values())
    rep['instructions'] = {'total': total, 'kinds': dict(kinds), 'unsupported': unsupported,
                           'unsupported_pct': round(100 * unsupported / max(total, 1), 4),
                           'by_reason': dict(reasons.most_common(40)), 'by_mnemonic': dict(mnems.most_common(40)),
                           'examples': {k: examples[k] for k, _ in mnems.most_common(40)}}

    # C size: emit every Nth function and scale by instructions
    order = sorted(entries)
    picked = order[::max(1, sample)]
    c_bytes, c_insns = 0, 0
    for a in picked:
        end, labels, tables = entries[a]
        lines = m.emit_function(a, end, labels, tables, {})
        c_bytes += sum(len(l) + 1 for l in lines)
        c_insns += (end - a) // 4
    est = c_bytes * total / max(c_insns, 1)
    rep['c_estimate'] = {'sampled_functions': len(picked), 'bytes_per_instruction': round(c_bytes / max(c_insns, 1), 1),
                         'estimated_mb': round(est / 1e6, 1)}
    rep['nvn_requested'] = sorted(nvn_requested(n))
    rep['seconds'] = round(time.time() - t0)
    return rep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exefs', nargs='?', default=os.path.join(ROOT, 'extracted', 'exefs'))
    ap.add_argument('--modules', help='comma-separated (default: every module of the layout)')
    ap.add_argument('--sample', type=int, default=50, help='emit every Nth function for the C size estimate')
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    ap.add_argument('--json')
    args = ap.parse_args()

    present = sorted(f for f in os.listdir(args.exefs) if not f.endswith('.npdm'))
    print(f'exefs: {args.exefs}\n  files: {present}')
    mods = layout.compute(args.exefs)
    bases = {n: b for n, b, s, i in mods}
    print('layout (tools/recomp/layout.py, what `make gen` and the runtime use):')
    layout.show(mods)
    why = layout.problems(mods)
    print(f'  {"CANNOT LOAD: " + "; ".join(why) if why else "loads"}')
    other = [f for f in present if f not in bases and f != 'rtld']
    if other:
        print(f'  NOTE: files that are not modules the runtime loads: {other}')

    reps = {}
    for name in (args.modules.split(',') if args.modules else list(bases)):
        path = os.path.join(args.exefs, name)
        if name not in bases:
            print(f'  {name}: {"missing" if not os.path.exists(path) else "not a module the runtime loads"}')
            continue
        print(f'\n== {name}: probing (this can take a while on a big module)', flush=True)
        r = probe_module(name, path, bases[name], args.simd, args.sample)
        reps[name] = r
        lay, d, ins, ce = r['layout'], r['discovery'], r['instructions'], r['c_estimate']
        print(f'  build {r["build_id"][:16]}  text {lay["text"] / 1e6:.1f} MB  ro {lay["rodata"] / 1e6:.1f} MB  '
              f'data {lay["data"] / 1e6:.1f} MB  bss {lay["bss"] / 1e6:.1f} MB  image {lay["image"] / 1e6:.1f} MB')
        print(f'  needed {r["needed"]}  imports {r["imports"]}  exports {r["exports"]}')
        print(f'  functions {d["functions"]} (boundaries {d["boundaries"]}, rounds {d["rounds"]}, {d["seconds"]}s)'
              f'  stats {d["stats"]}')
        print(f'  instructions {ins["total"]}  unsupported {ins["unsupported"]} ({ins["unsupported_pct"]}%)')
        for mn, k in list(ins['by_mnemonic'].items())[:15]:
            print(f'    {k:8d}  {mn:24s} e.g. {ins["examples"][mn]}')
        print(f'  generated C estimate: {ce["estimated_mb"]} MB ({ce["bytes_per_instruction"]} B/instruction, '
              f'{ce["sampled_functions"]} functions sampled)')
        print(f'  top imports: {list(r["import_groups"].items())[:10]}')

    report = {'modules': reps, 'layout': [{'module': n, 'base': hex(b), 'end': hex(b + s), 'build_id': i}
                                          for n, b, s, i in mods], 'layout_problems': why}

    impl, stubs = nvn_implemented()
    wanted = set().union(*(set(r['nvn_requested']) for r in reps.values())) if reps else set()
    missing = sorted(wanted - impl)
    stubbed = sorted(wanted & stubs)
    report['nvn'] = {'requested': len(wanted), 'implemented': len(wanted & impl), 'stubbed': stubbed,
                     'missing': missing}
    print(f'\nNVN: game names {len(wanted)} entry points; runtime implements {len(wanted & impl)} '
          f'({len(stubbed)} of them as empty stubs); missing {len(missing)}')
    for nm in missing[:40]:
        print(f'    missing {nm}')

    if args.json:
        with open(args.json, 'w') as f:
            json.dump(report, f, indent=1)
        print(f'\nreport: {args.json}')


if __name__ == '__main__':
    main()
