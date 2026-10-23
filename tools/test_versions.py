"""The version registry's consistency: tools/game_symbols.py, tools/patches.py, runtime/gameversions.h and the names
the runtime looks up agree, and every address is inside main.

    python3 tools/test_versions.py

Fails when runtime/gameversions.h is stale (run tools/gen_versions.py), when a build in one table is missing from the
other, when the runtime asks for a name a build does not define, or when a build's address is outside main's image.
"""
import glob
import os
import re
import sys

import game_symbols
import gen_versions
from patches import PATCH_BUILDS

TOOLS = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(TOOLS, '..', 'runtime')
SYM_RE = re.compile(r'rt_game_sym\("([a-z0-9_]+)"\)')

checks = 0


def check(cond, what):
    global checks
    if not cond:
        sys.exit(f'FAIL: {what}')
    checks += 1


def runtime_sources():
    return sorted(set(glob.glob(os.path.join(RUNTIME, '*.c')) + glob.glob(os.path.join(RUNTIME, '**', '*.c'),
                                                                        recursive=True) +
                      glob.glob(os.path.join(RUNTIME, '**', '*.mm'), recursive=True)))


def runtime_symbols():
    """{name: [where it is asked for]} of the names runtime/ looks up."""
    out = {}
    for path in runtime_sources():
        with open(path) as f:
            for n, line in enumerate(f, 1):
                for name in SYM_RE.findall(line):
                    out.setdefault(name, []).append(f'{os.path.relpath(path, RUNTIME)}:{n}')
    return out


def layout_with(main_size):
    return [('main', 0x7000000000, main_size, 'de0b6ac4efc1dda7')]


def main():
    # ---- the header the app is built with
    path = os.path.normpath(gen_versions.HEADER)
    check(os.path.exists(path), f'{path} exists (run python3 tools/gen_versions.py)')
    with open(path) as f:
        check(f.read() == gen_versions.header(), f'{path} is up to date (run python3 tools/gen_versions.py)')

    # ---- the two tables of builds
    check(list(game_symbols.VERSIONS) == list(PATCH_BUILDS), 'tools/patches.py has a table for every version')

    # ---- every build defines the same names, so the runtime needs no per-version path
    names = {bid: [n for n, _ in v['symbols'].items()] for bid, v in game_symbols.VERSIONS.items()}
    first, *rest = names
    for bid in rest:
        check(names[bid] == names[first], f'{game_symbols.VERSIONS[bid]["display"]}: same symbols as '
                                         f'{game_symbols.VERSIONS[first]["display"]}')
        for n in names[first]:
            check(game_symbols.VERSIONS[bid]['symbols'][n][0] == game_symbols.VERSIONS[first]['symbols'][n][0],
                  f'{bid}: {n} is of the same kind in both builds')

    # ---- the patch code's own addresses
    var_names = [(bid, sorted(v['vars'])) for bid, v in PATCH_BUILDS.items()]
    for bid, names_here in var_names[1:]:
        check(names_here == var_names[0][1], f'{bid}: the patches embed the same addresses')
    for bid, v in PATCH_BUILDS.items():
        check(v['vars']['title_message_file'] == game_symbols.values(bid)['title_message_file'],
              f'{bid}: the patches read the game addresses from tools/game_symbols.py')

    # ---- what the runtime looks up
    wanted = runtime_symbols()
    check(wanted, 'runtime/ asks for game symbols through rt_game_sym')
    for bid, v in game_symbols.VERSIONS.items():
        for name, where in wanted.items():
            check(name in v['symbols'], f'{v["display"]} ({bid}): no {name} (asked for in {", ".join(where)})')

    # ---- addresses inside main, and the layout check
    for bid, v in game_symbols.VERSIONS.items():
        syms = game_symbols.checked([('main', 0x7000000000, 0x1e98000, bid)])
        check([n for n, _ in syms] == names[bid], f'{v["display"]}: checked() gives its symbols')
    check(game_symbols.checked(layout_with(0x1d37000))[0][0] == 'pause_state', 'checked() of a layout')
    for bad, why in ((layout_with(0x1000), 'outside main'),
                     ([('sdk', 0x7000000000, 0x1000, 'cd57b23fa4bbad65')], 'no main module'),
                     ([('main', 0x7000000000, 0x1d37000, '0011223344556677')], 'unsupported main build id')):
        try:
            game_symbols.checked(bad)
            check(False, f'checked() refuses a layout ({why})')
        except ValueError as e:
            check(why in str(e), f'checked() says "{why}" ({e})')

    print(f'ok: {checks} checks')


if __name__ == '__main__':
    main()
