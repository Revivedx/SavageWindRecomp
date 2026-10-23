"""Guest addresses of the exefs modules: the one place that decides them.

The Switch loader puts a process's modules back to back in load order (rtld, main, subsdk0..9, sdk), each one as
big as its image (text, rodata, data + bss) rounded up to a 4 KiB page, from a 2 MiB-aligned start. Savage Wind does
the same without rtld, whose work the runtime does itself (runtime/loader.c, runtime/main.c), so main starts the
module area at MAIN_BASE and the modules may be any size the area holds.

The generated code has these addresses baked in, so everything else follows what this computes:
- `gen.py layout` / `gen.py all` write it next to the generated code: layout.txt (read back by gen.py for each
  module and by the start screen's build, runtime/gamebuild.c) and layout.c, compiled into the game code library;
- runtime/loader.c loads each module where the library's layout says (runtime/gamelib.h);
- sw_probe.py reports it.

Usage: layout.py <exefs_dir> [outdir]   (prints the layout; with outdir it also writes layout.txt and layout.c)
"""
import os
import sys

from nso import header

MODULES_LO = 0x7000000000  # runtime/rt.h MAIN_BASE (2 MiB aligned)
MODULES_HI = 0x7080000000  # runtime/rt.h MODULES_END: the loader refuses a module past it
PAGE = 0x1000
LOAD_ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


class LayoutError(Exception):
    pass


def image_size(h):
    """Bytes a module takes in memory (nso.header): its segments' furthest end, page-rounded."""
    end = max(h['text'][0] + h['text'][1], h['rodata'][0] + h['rodata'][1], h['data'][0] + h['data'][1] + h['bss'])
    return (end + PAGE - 1) & ~(PAGE - 1)


def compute(exefs):
    """[(name, base, size, build_id)] of the modules in an exefs folder, in load order."""
    out, base = [], MODULES_LO
    for name in LOAD_ORDER:
        path = os.path.join(exefs, name)
        if os.path.isfile(path):
            h = header(path)
            out.append((name, base, image_size(h), h['build_id']))
            base += out[-1][2]
    return out


def problems(layout):
    """Why the runtime could not load this layout (empty: it can)."""
    names = [m[0] for m in layout]
    why = [f'no {n} module' for n in ('main', 'sdk') if n not in names]
    end = layout[-1][1] + layout[-1][2] if layout else MODULES_LO
    if end > MODULES_HI:
        why.append(f'the modules end at {end:#x}, past {MODULES_HI:#x} (MODULES_END in runtime/rt.h)')
    return why


def checked(exefs):
    """compute(), or LayoutError if the runtime could not load it."""
    layout = compute(exefs)
    why = problems(layout)
    if why:
        raise LayoutError(f'{exefs}: ' + '; '.join(why))
    return layout


def text(layout):
    return ('# module layout (tools/layout.py), load order: name, guest base, image size, NSO build id\n' +
            ''.join(f'{n} {b:#x} {s:#x} {i}\n' for n, b, s, i in layout))


def read(path):
    """The layout from a layout.txt."""
    out = []
    for line in open(path):
        if line.strip() and not line.startswith('#'):
            n, b, s, i = line.split()
            out.append((n, int(b, 16), int(s, 16), i))
    return out


def c_source(layout, symbols=()):
    """layout.c: the layout inside the game code library, with each module's function table (runtime/gamelib.h) and
    the game addresses the runtime uses (tools/game_symbols.py, read through rt_game_sym)."""
    lines = ['// Module layout of this game code (tools/layout.py). Generated.\n#include "gamelib.h"\n\n']
    for n, *_ in layout:
        lines.append(f'extern const uint64_t {n}_fn_addrs[], {n}_fn_count;\nextern const GuestFn {n}_fn_ptrs[];\n')
    lines.append('\nconst GameLibModule game_modules[] = {\n')
    for n, b, s, i in layout:
        lines.append(f'    {{"{n}", {b:#x}ull, {s:#x}ull, "{i}", {n}_fn_addrs, {n}_fn_ptrs, &{n}_fn_count}},\n')
    lines.append(f'}};\nconst uint32_t game_module_count = {len(layout)};\n')
    lines.append('\nconst GameLibSymbol game_symbols[] = {\n')
    for name, (kind, value) in symbols:
        lines.append(f'    {{"{name}", {value:#x}ull}},\n')
    if not symbols:  # (C has no empty initializer)
        lines.append('    {0, 0},\n')
    lines.append(f'}};\nconst uint32_t game_symbol_count = {len(symbols)};\n')
    return ''.join(lines)


def write_if_changed(path, s):  # (an unchanged file keeps its date: make doesn't recompile it)
    try:
        with open(path) as f:
            if f.read() == s:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(s)


def write(layout, outdir, symbols=()):
    os.makedirs(outdir, exist_ok=True)
    write_if_changed(os.path.join(outdir, 'layout.txt'), text(layout))
    write_if_changed(os.path.join(outdir, 'layout.c'), c_source(layout, symbols))


def show(layout):
    for n, b, s, i in layout:
        print(f'  {n:8s} {b:#x}-{b + s:#x}  {s / 2**20:7.1f} MiB  build {i[:16]}')


if __name__ == '__main__':
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    lay = compute(sys.argv[1])
    show(lay)
    why = problems(lay)
    if why:
        sys.exit('layout: ' + '; '.join(why))
    if len(sys.argv) == 3:
        import game_symbols
        try:
            write(lay, sys.argv[2], game_symbols.checked(lay))
        except ValueError as e:
            sys.exit(f'layout: {e}')
