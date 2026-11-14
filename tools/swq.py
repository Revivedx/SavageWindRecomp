#!/usr/bin/env python3
"""swq.py CAPTURE --exe EXE [--thread nvn-queue] [--top 40]: per-thread leaf attribution from a swprof capture.

Leaf = innermost frame. Runtime frames are named from the exe; system/driver frames by module and the first runtime caller."""
import argparse, bisect, collections, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swprof

ap = argparse.ArgumentParser()
ap.add_argument('capture')
ap.add_argument('--exe')
ap.add_argument('--thread', default='nvn-queue')
ap.add_argument('--top', type=int, default=40)
a = ap.parse_args()
cap = swprof.Capture(a.capture)
mods = sorted(cap.modules)
starts = [m[0] for m in mods]
rt = next((m for m in mods if m[3] == swprof.MOD_RUNTIME), None)
sym = swprof.Symbols(a.exe, rt)


def module_of(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < mods[i][0] + mods[i][1]:
        return mods[i]
    return None


tids = {tid for (n, tid) in cap.threads if n == a.thread}
frames = len(cap.frame_marks) - 1
leaf = collections.Counter()
incl = collections.Counter()
n = 0
for t, tid, handle, pcs in cap.samples:
    if tid not in tids or not pcs:
        continue
    n += 1
    ms = [module_of(x) for x in pcs]
    if ms[0] and ms[0][3] == swprof.MOD_RUNTIME:
        leaf['rt:' + sym.name(pcs[0])] += 1
    elif ms[0] and ms[0][3] == swprof.MOD_GAME:
        leaf['game'] += 1
    else:
        caller = next((sym.name(pcs[i]) for i in range(len(pcs)) if ms[i] and ms[i][3] == swprof.MOD_RUNTIME), '?')
        leaf['sys:%s <- %s' % (os.path.basename(ms[0][4]) if ms[0] else '?', caller)] += 1
    seen = set()
    for i, x in enumerate(pcs):
        if ms[i] and ms[i][3] == swprof.MOD_RUNTIME:
            nm = sym.name(x)
            if nm not in seen:
                seen.add(nm)
                incl[nm] += 1
interval = 1.35
print('thread %s: %d samples over %d frames; each sample = ~%.2f ms/frame/1000' % (a.thread, n, frames, interval))
tot = n * interval / frames
print('total %.2f ms/frame' % tot)
print('--- leaf (self) ms/frame')
for k, v in leaf.most_common(a.top):
    print('%7.3f %5.1f%%  %s' % (v * interval / frames, 100 * v / n, k))
print('--- inclusive (anywhere in stack) ms/frame')
for k, v in incl.most_common(a.top):
    print('%7.3f %5.1f%%  %s' % (v * interval / frames, 100 * v / n, k))
