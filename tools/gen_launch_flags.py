#!/usr/bin/env python3
"""Launch options: the table of environment flags the start screen suggests (runtime/launch_flags_table.c).

Scans runtime/ for every "SAVAGEWIND_*" name the code passes to getenv() or to a helper such as env_int(), works out
what kind of value each takes, and joins that with tools/launch_flags_curated.txt (which flags can't be added from the
start screen, which are suggested first, and what each does in one line).

usage: tools/gen_launch_flags.py           write runtime/launch_flags_table.c
       tools/gen_launch_flags.py --check   exit 1 if the checked-in table is stale (make test-launch-flags)
       tools/gen_launch_flags.py --list    print the table as text

Kinds, from how the code reads the flag:
  switch  every read only asks "is it set?": any value turns it on, so it can only be turned off by removing it
  number  some read parses a number (atoi, env_int, ...): 0 can turn it off
  text    anything else (a path, a name, a mode word)
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'runtime', 'launch_flags_table.c')
CURATED = os.path.join(ROOT, 'tools', 'launch_flags_curated.txt')
SCAN_EXT = ('.c', '.h', '.cpp', '.m', '.mm')
SKIP = {'launch_flags_table.c'}  # (this table itself)

NAME = r'SAVAGEWIND_[A-Z0-9_]+'
LITERAL = re.compile(r'[(,]\s*"(' + NAME + r')"')
NUMERIC = re.compile(r'\b(atoi|atol|atoll|atof|strtol|strtoul|strtoll|strtoull|strtod|sscanf|env_int|env_factor|env_float|env_long)\b')
TEXTUSE = re.compile(r'\b(strcmp|strncmp|strcasecmp|strstr|strchr|strdup|snprintf|sprintf|fopen|open|stat|memcmp)\b')
# a read that only asks whether the flag is set
SWITCH_USES = [
    r'getenv\(\s*"{n}"\s*\)\s*(!=|==)\s*NULL',
    r'!\s*getenv\(\s*"{n}"\s*\)',
    r'\(\s*getenv\(\s*"{n}"\s*\)\s*[)&|?]',
    r'getenv\(\s*"{n}"\s*\)\s*(&&|\|\||\?)',
    r'\bENV\(\s*"{n}"\s*\)\s*(!=|==)\s*NULL',
]
DEV_WORDS = {'DUMP', 'DUMPS', 'DUMPWB', 'TRACE', 'LOG', 'PROF', 'GPUPROF', 'FNPROF', 'SHOT', 'DEBUG', 'DBG', 'TEST', 'SAMPLE',
             'WATCH', 'VERIFY', 'STATS', 'CSV', 'PROBE', 'TIMELINE', 'AT', 'VIEW', 'CHECKPOINTS', 'VALIDATION', 'COUNTER',
             'SCRIPT', 'SOLO', 'WAV', 'SCAN', 'REPORT', 'TEXDUMP', 'SCREENSHOT', 'SCREENSHOTS', 'DEV', 'CONTROL', 'CLOCK',
             'REBOOT', 'WINDOW', 'OPT', 'SEED', 'GLYPH', 'ONESHOT'}


def scan():
    names = {}  # name -> list of source lines that mention it as a literal
    for base, _, files in os.walk(os.path.join(ROOT, 'runtime')):
        for f in sorted(files):
            if not f.endswith(SCAN_EXT) or f in SKIP:
                continue
            with open(os.path.join(base, f), encoding='utf-8', errors='replace') as fh:
                for line in fh:
                    stripped = line.lstrip()
                    if stripped.startswith('//'):
                        continue
                    for m in LITERAL.finditer(line):
                        names.setdefault(m.group(1), []).append(line)
    return names


def kind_of(name, lines):
    if any(NUMERIC.search(l) for l in lines):
        return 'number'
    uses = [re.compile(p.format(n=name)) for p in SWITCH_USES]
    if all(any(u.search(l) for u in uses) and not TEXTUSE.search(l) for l in lines):
        return 'switch'
    return 'text'


def curated():
    out = {}
    with open(CURATED, encoding='utf-8') as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip('\n')
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            head, _, rest = line.partition('|')
            parts = head.split()
            example, _, desc = rest.partition('|')
            if len(parts) != 2 or parts[0] not in ('internal', 'launcher', 'build', 'play') or not desc.strip():
                sys.exit('%s:%d: expected "group NAME | example | description"' % (CURATED, n))
            if parts[1] in out:
                sys.exit('%s:%d: %s is listed twice' % (CURATED, n, parts[1]))
            if '\u2014' in desc or '\u2013' in desc or ' - ' in desc:
                sys.exit('%s:%d: no dashes in descriptions' % (CURATED, n))
            out[parts[1]] = (parts[0], example.strip(), desc.strip())
    return out


def group_of(name, cur):
    if name in cur:
        return cur[name][0]
    words = set(name[len('SAVAGEWIND_'):].split('_'))
    return 'dev' if words & DEV_WORDS else 'other'


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def build():
    found = scan()
    cur = curated()
    for n, (g, _, _) in cur.items():
        if g == 'play' and n not in found:
            sys.exit('%s lists %s as a play flag, but runtime/ no longer reads it' % (CURATED, n))
    rows = []
    for name in sorted(set(found) | set(cur)):
        g = group_of(name, cur)
        kind = kind_of(name, found[name]) if name in found else 'text'
        example, desc = (cur[name][1], cur[name][2]) if name in cur else ('', '')
        rows.append((name, kind, g, example, desc))
    lines = ['// Generated by tools/gen_launch_flags.py from runtime/ and tools/launch_flags_curated.txt. Do not edit by hand.',
             '// (make test-launch-flags fails when this is stale.)',
             '#include "launch_flags.h"', '',
             'const LfInfo LF_TABLE[] = {']
    for name, kind, g, example, desc in rows:
        lines.append('    {%s, LF_KIND_%s, LF_GROUP_%s, %s, %s},' % (cstr(name), kind.upper(), g.upper(), cstr(example), cstr(desc)))
    lines += ['};', 'const int LF_TABLE_N = %d;' % len(rows), '']
    return '\n'.join(lines), rows


def main():
    text, rows = build()
    if '--list' in sys.argv:
        for r in rows:
            print('%-8s %-8s %s' % (r[2], r[1], r[0]))
        return 0
    if '--check' in sys.argv:
        try:
            with open(OUT, encoding='utf-8') as fh:
                have = fh.read()
        except OSError:
            have = ''
        if have != text:
            print('runtime/launch_flags_table.c is stale: run python3 tools/gen_launch_flags.py', file=sys.stderr)
            return 1
        print('launch flag table is up to date (%d names)' % len(rows))
        return 0
    with open(OUT, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(text)
    print('wrote %s (%d names)' % (os.path.relpath(OUT, ROOT), len(rows)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
