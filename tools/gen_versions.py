"""Writes runtime/gameversions.h from tools/game_symbols.py (the start screen's list of supported versions).

The app build has no Python, so the header is committed; `--check` exits 1 when it is stale (tools/test_versions.py).
"""
import os
import sys

from game_symbols import VERSIONS

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'runtime', 'gameversions.h')


def header():
    lines = ['// Versions Savage Wind can recompile and run. Generated from tools/game_symbols.py by tools/gen_versions.py;',
             '// tools/test_versions.py fails when this file is stale.',
             '#pragma once',
             'typedef struct { const char *id; const char *display; } KnownGameVersion;',
             'static const KnownGameVersion known_game_versions[] = {']
    for build_id, v in VERSIONS.items():
        lines.append(f'    {{"{build_id}", "{v["display"]}"}},')
    lines += ['};',
              f'static const unsigned known_game_version_count = {len(VERSIONS)};',
              '']
    return '\n'.join(lines)


def main():
    text = header()
    if '--check' in sys.argv[1:]:
        try:
            with open(HEADER) as f:
                if f.read() == text:
                    return
        except FileNotFoundError:
            pass
        sys.exit(f'{os.path.normpath(HEADER)} is stale: run python3 tools/gen_versions.py')
    with open(HEADER, 'w') as f:
        f.write(text)


if __name__ == '__main__':
    main()
