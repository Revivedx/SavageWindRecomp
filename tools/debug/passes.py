#!/usr/bin/env python3
"""Summarise a SAVAGEWIND_DEBUG_FRAME log (build/run_shot.log): consecutive draws grouped by (fragment shader hash,
render target), in order: index range, count, rt, depth, first draw's sizes."""
import re, sys
log = open(sys.argv[1] if len(sys.argv) > 1 else 'build/run_shot.log').read().split('\n')
h = None
rows = []
for l in log:
    if l.startswith('[fshash]'):
        h = l.split()[-1]
    elif l.startswith('[draw]'):
        rt = re.search(r'\| rt (\d+) (\S+)', l)
        cnt = re.search(r'count (\d+)', l)
        rows.append((h, rt.group(2) if rt else '?', int(cnt.group(1)) if cnt else 0))
grp = []
for i, (fh, rt, c) in enumerate(rows):
    if grp and grp[-1][0] == fh and grp[-1][1] == rt:
        grp[-1][3] += 1
    else:
        grp.append([fh, rt, i, 1, c])
for fh, rt, i, n, c in grp:
    print(f'{i:5} x{n:<4} {fh} rt {rt:22} count {c}')
