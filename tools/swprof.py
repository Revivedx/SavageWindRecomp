#!/usr/bin/env python3
"""Reads a capture of the on-demand profiler (runtime/prof.c) and says where the time went.

Usage:
  swprof.py [capture.swprof] [--top N] [--slow MS] [--json FILE] [--csv FILE] [--frames FIRST:LAST] [--exe RUNTIME_BINARY]

Without a file the newest profile/swprof-*.swprof is used. Python 3.8, standard library only.

The report has: the frames (rate, percentiles, the slow ones with what each thread was doing), the zones
(SW_ZONE) as a tree per thread, the added-up work (SW_ACC) as a tree per thread, the counters, and the top places by
self time, and, when the sampler ran, where the threads sat: game code, the SWR runtime (named per function when --exe
points at the runtime binary of the run), the driver and system, or waiting. The game code is one total: no game
function is ever looked up or named. Times are milliseconds per frame unless a heading says otherwise.
"""
import argparse
import bisect
import collections
import glob
import json
import math
import os
import shutil
import struct
import subprocess
import sys

HEADER = struct.Struct('<8sIIQQQQQIIII')
THREAD = struct.Struct('<IIQ32s')
CHUNK = struct.Struct('<II')
REC = struct.Struct('<QQQII')
MODULE = struct.Struct('<QQQI196s')

ZONE, COUNTER, FRAME, ACC, GPU, SAMPLE, STACK = 1, 2, 3, 4, 5, 6, 7
MOD_RUNTIME, MOD_GAME, MOD_OTHER = 1, 2, 3
GPU_IDLE = '(gpu idle)'
# zones in which a thread is waiting for something, not working: a sample inside one counts as waiting
WAIT_ZONES = {'kernel wait', 'kernel wait (handles)', 'guest sleep', 'gpu_wait', 'present pace', 'wait for recorder submit',
              'recorder drain', 'recorder ring full'}
# Runtime functions that only wait (for a kernel object, for work, for one of the four cores). A sample whose stack is in
# the system below one of them is waiting even with no wait zone around it: a wait that began before the capture did has
# no zone (its stamp was not taken), and some waits have none.
WAIT_FUNCS = {'kwait', 'block_current', 'guest_sleep', 'queue_wait', 'queue_thread', 'rec_drain', 'core_acquire', 'vk_wait_seq'}
COPYRIGHT_NOTE = ('Reminder: the game code is shown as one total on purpose. Do not put game addresses, game names or '
                  'generated code into pull requests or issues.')


class Capture:
    """sites {id: name}, threads [name], zones/acc per thread, counters, frame marks; all times in ms from the start."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            data = f.read()
        (magic, version, recsize, t0, n0, t1, n1, unix0, nsites, nthreads, nchunks, nmodules) = HEADER.unpack_from(data, 0)
        if magic != b'SWPROF01' or version != 1 or recsize != REC.size:
            raise SystemExit(f'{path}: not a capture this tool reads (magic {magic!r}, version {version})')
        self.path = path
        self.unix_start = unix0
        pos = HEADER.size
        self.sites = {}
        for _ in range(nsites):
            sid, ln = struct.unpack_from('<IH', data, pos)
            pos += 6
            self.sites[sid] = data[pos:pos + ln].decode('utf-8', 'replace')
            pos += ln
        self.threads = []
        for _ in range(nthreads):
            _, _, tid, name = THREAD.unpack_from(data, pos)
            pos += THREAD.size
            self.threads.append((name.split(b'\0')[0].decode('utf-8', 'replace') or 'thread', tid))
        self.modules = []  # (base, size, bias, kind, name)
        for _ in range(nmodules):
            base, size, bias, kind, name = MODULE.unpack_from(data, pos)
            pos += MODULE.size
            self.modules.append((base, size, bias, kind, name.split(b'\0')[0].decode('utf-8', 'replace')))
        span_ns = max(n1 - n0, 1)
        self.ticks_per_ms = max((t1 - t0) / span_ns * 1e6, 1e-9)
        self.ticks0, self.ticks1 = t0, t1
        self.duration_ms = span_ns / 1e6
        # per thread: lists of (start, end, site) zones and (frame, site, calls, ticks) accumulations
        self.zones = [[] for _ in self.threads]
        self.acc = [[] for _ in self.threads]
        self.counters = collections.defaultdict(list)  # name -> [(ms, value)]
        self.frame_marks = []  # (ms, frame number)
        self.gpu = []  # (start, end, site, draws)
        self.samples = []  # (ms, os thread id, kernel handle, [addresses innermost first])
        self.sample_depth = 0
        self.records = 0
        ms = lambda t: (t - t0) / self.ticks_per_ms
        for chunk in range(nchunks):
            available = len(data) - pos
            if available < CHUNK.size:
                raise SystemExit(f'{path}: truncated chunk {chunk + 1}/{nchunks} header: '
                                 f'expected {CHUNK.size} bytes, found {available} bytes')
            thread, n = CHUNK.unpack_from(data, pos)
            pos += CHUNK.size
            size = n * REC.size
            available = len(data) - pos
            if available < size:
                raise SystemExit(f'{path}: truncated chunk {chunk + 1}/{nchunks}: '
                                 f'expected {n} records ({size} bytes), found {available} bytes')
            for a, b, val, site, kind in REC.iter_unpack(data[pos:pos + size]):
                k = kind & 0xff
                if k == SAMPLE:
                    self.samples.append([ms(a), b, val, []])
                    self.sample_depth = kind >> 8
                    continue
                if k == STACK:  # (an address, not a time: before the check below)
                    if self.samples and len(self.samples[-1][3]) < self.sample_depth:
                        self.samples[-1][3].extend((a, b, val))
                        del self.samples[-1][3][self.sample_depth:]
                    continue
                if a < t0:
                    continue  # began before the capture
                if k == ZONE and thread < len(self.zones):
                    self.zones[thread].append((ms(a), ms(b), site))
                elif k == ACC and thread < len(self.acc):
                    self.acc[thread].append((val, site, kind >> 8, b / self.ticks_per_ms))
                elif k == COUNTER:
                    self.counters[self.sites.get(site, '?')].append((ms(a), b))  # the value is in t1
                elif k == FRAME:
                    self.frame_marks.append((ms(a), val))
                elif k == GPU:
                    self.gpu.append((ms(a), ms(b), site, val & 0xffffffff))
            pos += size
            self.records += n
        self.frame_marks.sort()
        self.gpu.sort()
        if self.gpu:  # the GPU's rows are a thread of their own: flat, one after another
            self.threads.append(('gpu', 0))
            self.zones.append([(a, b, site) for a, b, site, _ in self.gpu])
            self.acc.append([])


def percentile(sorted_values, p):
    """Nearest rank."""
    if not sorted_values:
        return 0.0
    i = max(0, min(len(sorted_values) - 1, math.ceil(p / 100 * len(sorted_values)) - 1))
    return sorted_values[i]


class Frames:
    def __init__(self, cap, first=None, last=None):
        marks = cap.frame_marks
        self.starts = [m[0] for m in marks]
        self.numbers = [m[1] for m in marks]
        # frame i lasts from mark i to mark i+1; the last mark has no end
        self.items = []
        for i in range(len(marks) - 1):
            n = marks[i][1]
            if (first is not None and n < first) or (last is not None and n > last):
                continue
            self.items.append((i, n, marks[i][0], marks[i + 1][0] - marks[i][0]))
        self.ms = sorted(it[3] for it in self.items)

    def index_at(self, t):
        """Frame mark index whose frame holds time t (-1 before the first mark)."""
        return bisect.bisect_right(self.starts, t) - 1


def build_tree(zones, names, span=(-math.inf, math.inf)):
    """zones: (start, end, site) of one thread -> {path tuple: [calls, inclusive ms, self ms, max ms]}; also each
    self-time intervals (start, end, path) for attributing to frames. Build nesting before clipping to span."""
    zones = sorted(zones, key=lambda z: (z[0], -z[1]))
    stack = []  # [end, path, direct child intervals, start]
    stats = {}
    events = []

    def close(item):
        end, path, children, start = item
        start, end = max(start, span[0]), min(end, span[1])
        if end <= start:
            return
        dur = end - start
        cursor, own = start, 0.0
        for a, b in children:
            a, b = max(a, start), min(b, end)
            if b <= a:
                continue
            if a > cursor:
                events.append((cursor, a, path))
                own += a - cursor
            cursor = max(cursor, b)
        if cursor < end:
            events.append((cursor, end, path))
            own += end - cursor
        s = stats.setdefault(path, [0, 0.0, 0.0, 0.0])
        s[0] += 1
        s[1] += dur
        s[2] += own
        s[3] = max(s[3], dur)

    for start, end, site in zones:
        while stack and start >= stack[-1][0] - 1e-9:
            close(stack.pop())
        name = names.get(site, '?')
        path = (stack[-1][1] if stack else ()) + (name,)
        if stack:
            stack[-1][2].append((start, end))
        stack.append([end, path, [], start])
    while stack:
        close(stack.pop())
    return stats, events


def acc_tree(records, names):
    """records: (frame, site, calls, ms) -> {name: [calls, ms]} with names as '/' paths."""
    out = {}
    for _, site, calls, ms in records:
        s = out.setdefault(names.get(site, '?'), [0, 0.0])
        s[0] += calls
        s[1] += ms
    return out


def self_times(tree):
    """{path: [calls, incl]} where path is a '/' name -> {path: self ms}: inclusive minus the direct children's."""
    child = collections.defaultdict(float)
    for p, (_, ms) in tree.items():
        if '/' in p:
            child[p.rsplit('/', 1)[0]] += ms
    return {p: max(ms - child.get(p, 0.0), 0.0) for p, (_, ms) in tree.items()}


class Symbols:
    """Function names of the runtime binary, from nm. A module address becomes a binary address by the module's bias
    (ELF) or its base minus the PE ImageBase; an address with no name stays 'runtime+0x...'. Only the runtime is ever
    named: the game library's functions are never looked up."""

    def __init__(self, exe, module):
        self.addrs, self.names, self.delta = [], [], 0
        self.base = module[0] if module else 0
        self.note = ''
        if not exe:
            self.note = 'no runtime binary given (--exe): function names are runtime+offset'
            return
        try:
            with open(exe, 'rb') as f:
                head = f.read(0x1000)
        except OSError as e:
            self.note = f'cannot read {exe}: {e.strerror}'
            return
        if head[:2] == b'MZ':  # PE: nm lists ImageBase + RVA
            pe = struct.unpack_from('<I', head, 0x3c)[0]
            image_base = struct.unpack_from('<Q', head, pe + 24 + 24)[0] if pe + 56 < len(head) else 0
            self.delta = (module[0] if module else 0) - image_base
        else:  # ELF / Mach-O: the load bias
            self.delta = module[2] if module else 0
        tools = [t for t in ('llvm-nm', 'llvm-nm-20', 'llvm-nm-19', 'llvm-nm-18', 'llvm-nm-17', 'llvm-nm-16', 'nm')
                 if shutil.which(t)]
        if not tools:
            self.note = 'nm not found: function names are runtime+offset'
            return
        # (the plain nm of a Linux host can't read a Windows binary, llvm-nm can: take the first that finds symbols)
        for tool in tools:
            try:
                out = subprocess.run([tool, '-n', '--defined-only', exe], capture_output=True, text=True,
                                     timeout=120).stdout
            except (OSError, subprocess.SubprocessError) as e:
                self.note = f'{tool} failed: {e}'
                continue
            for line in out.splitlines():
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[1] in ('t', 'T', 'w', 'W', 'i', 'I'):
                    try:
                        self.addrs.append(int(parts[0], 16))
                    except ValueError:
                        continue
                    self.names.append(parts[2])
            if self.addrs:
                self.note = ''
                break
        if not self.addrs:
            self.note = 'the binary has no symbols: function names are runtime+offset'

    def name(self, addr):
        a = addr - self.delta
        i = bisect.bisect_right(self.addrs, a) - 1
        if i >= 0 and a - self.addrs[i] < (1 << 20):
            return self.names[i]
        return f'runtime+0x{addr - self.base:x}'


CATEGORIES = ('game', 'runtime', 'driver/system', 'waiting')


def wait_intervals(cap, ti):
    """Merged (start, end) of the zones on thread ti where it only waits."""
    iv = sorted((a, b) for a, b, site in cap.zones[ti] if cap.sites.get(site) in WAIT_ZONES)
    merged = []
    for a, b in iv:
        if merged and a <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return merged


def sampler_summary(cap, fr, span, nframes, symbols):
    """Where the threads were, by sample: game code / the runtime / the driver and system / waiting. Runtime functions
    by self (top frame) and inclusive (anywhere in the stack); what the runtime was doing when a sample sat in the
    driver or system."""
    if not cap.samples:
        return {}
    mods = sorted(cap.modules)
    starts = [m[0] for m in mods]

    def module_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < mods[i][0] + mods[i][1]:
            return mods[i]
        return None

    by_tid = {}
    for ti, (name, tid) in enumerate(cap.threads):
        if name != 'gpu':
            by_tid[tid] = ti
    waits = {}
    split = collections.defaultdict(lambda: collections.Counter())  # thread -> category -> samples
    times = collections.defaultdict(list)
    rt_self, rt_incl, rt_callers = collections.Counter(), collections.Counter(), collections.Counter()
    rt_waits = collections.Counter()  # waits found by the stack (WAIT_FUNCS), not by a zone
    per_frame = collections.defaultdict(collections.Counter)  # frame mark index -> category -> samples
    total = 0
    for t, tid, handle, pcs in cap.samples:
        if not (span[0] <= t < span[1]) or not pcs:
            continue
        ti = by_tid.get(tid)
        tname = cap.threads[ti][0] if ti is not None else f'os thread {tid}'
        if ti is not None and ti not in waits:
            waits[ti] = wait_intervals(cap, ti)
        times[tname].append(t)
        total += 1
        mk = [module_of(a) for a in pcs]
        top = mk[0]
        cat = 'waiting' if ti is not None and _inside(waits[ti], t) else \
            'game' if top and top[3] == MOD_GAME else 'runtime' if top and top[3] == MOD_RUNTIME else 'driver/system'
        split[tname][cat] += 1
        per_frame[fr.index_at(t)][cat] += 1
        if cat == 'waiting':
            continue
        rt = [pcs[i] for i in range(len(pcs)) if mk[i] and mk[i][3] == MOD_RUNTIME]
        if not rt:
            continue
        names = [symbols.name(a) for a in rt]
        if cat == 'driver/system' and names[0] in WAIT_FUNCS:
            split[tname][cat] -= 1
            split[tname]['waiting'] += 1
            per_frame[fr.index_at(t)][cat] -= 1
            per_frame[fr.index_at(t)]['waiting'] += 1
            rt_waits[names[0]] += 1
            continue
        if cat == 'runtime':
            rt_self[names[0]] += 1
        elif cat == 'driver/system':
            rt_callers[names[0]] += 1
        for n in set(names):
            rt_incl[n] += 1
    if not total:
        return {}
    # time between two samples of one thread, to turn counts into milliseconds
    gaps = sorted(b - a for ts in times.values() for a, b in zip(ts, ts[1:]) if b - a < 50)
    interval = percentile(gaps, 50) if gaps else 1.0

    def rows(counter, n=40):
        return [{'name': k, 'samples': v, 'percent': round(100 * v / total, 2),
                 'ms_per_frame': round(v * interval / nframes, 4)} for k, v in counter.most_common(n)]

    return {
        'samples': total, 'interval_ms': round(interval, 3), 'symbols_note': symbols.note,
        'threads': [{'name': n, 'samples': sum(c.values()),
                     **{k: round(100 * c[k] / sum(c.values()), 1) for k in CATEGORIES}}
                    for n, c in sorted(split.items(), key=lambda kv: -sum(kv[1].values()))],
        'runtime_self': rows(rt_self), 'runtime_inclusive': rows(rt_incl), 'runtime_callers_of_driver': rows(rt_callers),
        'runtime_waits': rows(rt_waits),
        '_per_frame': per_frame,
    }


def _inside(intervals, t):
    i = bisect.bisect_right(intervals, [t, math.inf]) - 1
    return i >= 0 and intervals[i][0] <= t < intervals[i][1]


def analyse(cap, first=None, last=None, exe=None):
    fr = Frames(cap, first, last)
    nframes = max(len(fr.items), 1)
    result = {'file': os.path.basename(cap.path), 'duration_s': round(cap.duration_ms / 1000, 3), 'events': cap.records,
              'frames': {}, 'threads': [], 'counters': {}, 'top_self': [], 'slow_frames': []}
    if fr.items:
        ms = fr.ms
        total = sum(ms)
        result['frames'] = {
            'count': len(ms), 'fps_avg': round(1000 * len(ms) / total, 2) if total else 0,
            'ms_avg': round(total / len(ms), 3), 'ms_p50': round(percentile(ms, 50), 3),
            'ms_p95': round(percentile(ms, 95), 3), 'ms_p99': round(percentile(ms, 99), 3), 'ms_max': round(ms[-1], 3),
            'over_33_ms': sum(1 for v in ms if v > 33.4), 'over_50_ms': sum(1 for v in ms if v > 50)}
    span = (fr.items[0][2], fr.items[-1][2] + fr.items[-1][3]) if fr.items else \
        (0.0, 0.0) if first is not None or last is not None else (0.0, cap.duration_ms)

    wanted = {it[1] for it in fr.items}
    frame_indices = {n: i for i, n, _, _ in fr.items}
    per_frame_self = collections.defaultdict(lambda: collections.defaultdict(float))  # frame mark index -> key -> ms
    top = collections.defaultdict(lambda: [0, 0.0])  # (thread, name) -> calls, self ms
    for ti, (tname, tid) in enumerate(cap.threads):
        stats, events = build_tree(cap.zones[ti], cap.sites, span)
        zone_frames = collections.defaultdict(lambda: collections.defaultdict(float))
        for start, end, path in events:
            if not fr.items:
                zone_frames[None][path] += end - start
            i = max(fr.index_at(start), 0)
            while i + 1 < len(fr.starts) and fr.starts[i] < end:
                overlap = min(end, fr.starts[i + 1]) - max(start, fr.starts[i])
                if overlap > 0:
                    zone_frames[i][path] += overlap
                i += 1
        accs = [a for a in cap.acc[ti] if a[0] in wanted] if fr.items or first is not None or last is not None else cap.acc[ti]
        at = acc_tree(accs, cap.sites)
        acc_frames = collections.defaultdict(list)
        for record in accs:
            acc_frames[frame_indices.get(record[0])].append(record)
        aself = collections.defaultdict(float)
        for i, records in acc_frames.items():
            frame_self = self_times(acc_tree(records, cap.sites))
            for p, ms in frame_self.items():
                aself[p] += ms
                if i is not None:
                    per_frame_self[i][(tname, p)] += ms
            # ACC has no intervals or parent links. Estimate its enclosing work from this thread/frame's
            # non-wait zone self time, proportionally and without allowing negative zone self time.
            zones = zone_frames[i]
            available = sum(ms for p, ms in zones.items() if p[-1] not in WAIT_ZONES)
            fraction = min(sum(frame_self.values()) / available, 1.0) if available else 0.0
            for p, ms in zones.items():
                if p[-1] in WAIT_ZONES:
                    continue
                removed = ms * fraction
                zones[p] = max(ms - removed, 0.0)
                stats[p][2] = max(stats[p][2] - removed, 0.0)
        for i, zones in zone_frames.items():
            if i is not None:
                for p, ms in zones.items():
                    per_frame_self[i][(tname, '/'.join(p))] += ms
        for p, selfms in aself.items():
            top[(tname, p)][0] += at[p][0]
            top[(tname, p)][1] += selfms
        for path, (calls, incl, selfms, mx) in stats.items():
            top[(tname, '/'.join(path))][0] += calls
            top[(tname, '/'.join(path))][1] += selfms
        busy = sum(v[1] for p, v in stats.items() if len(p) == 1)
        result['threads'].append({
            'name': tname, 'os_id': tid, 'busy_ms_per_frame': round(busy / nframes, 3),
            'zones': [{'path': '/'.join(p), 'calls_per_frame': round(v[0] / nframes, 2), 'incl_ms': round(v[1] / nframes, 4),
                       'self_ms': round(v[2] / nframes, 4), 'max_ms': round(v[3], 3)} for p, v in sorted(stats.items())],
            'acc': [{'path': p, 'calls_per_frame': round(v[0] / nframes, 1), 'incl_ms': round(v[1] / nframes, 4),
                     'self_ms': round(aself[p] / nframes, 4)} for p, v in sorted(at.items())]})
    for name, vals in cap.counters.items():
        vs = [v for t, v in vals if span[0] <= t < span[1]]
        if vs:
            result['counters'][name] = {'min': min(vs), 'avg': round(sum(vs) / len(vs), 2), 'max': max(vs), 'samples': len(vs)}
    # waiting is not work: a thread blocked in a wait zone is listed apart, so the list of places leads with work
    rows = [{'thread': k[0], 'name': k[1], 'calls_per_frame': round(v[0] / nframes, 2),
             'self_ms_per_frame': round(v[1] / nframes, 4)}
            for k, v in sorted(top.items(), key=lambda kv: -kv[1][1])]
    result['top_self'] = [r for r in rows if r['name'].rsplit('/', 1)[-1] not in WAIT_ZONES]
    result['top_waits'] = [r for r in rows if r['name'].rsplit('/', 1)[-1] in WAIT_ZONES]
    # slow frames: the worst few, with the places that took the most self time inside each
    worst = sorted(fr.items, key=lambda it: -it[3])[:10]
    for i, n, start, dur in worst:
        work = ((k, v) for k, v in per_frame_self.get(i, {}).items() if k[1].rsplit('/', 1)[-1] not in WAIT_ZONES)
        cause = sorted(work, key=lambda kv: -kv[1])[:6]
        result['slow_frames'].append({'frame': n, 'ms': round(dur, 2), 'at_s': round(start / 1000, 3),
                                      'top': [{'thread': k[0], 'name': k[1], 'ms': round(v, 2)} for k, v in cause]})
    result['gpu'] = gpu_summary(cap, fr, span, nframes)
    runtime = next((m for m in cap.modules if m[3] == MOD_RUNTIME), None)
    result['sampler'] = sampler_summary(cap, fr, span, nframes, Symbols(exe, runtime))
    per = result['sampler'].pop('_per_frame', {}) if result['sampler'] else {}
    for sf, (i, n, start, dur) in zip(result['slow_frames'], sorted(fr.items, key=lambda it: -it[3])[:10]):
        c = per.get(i)
        if c:
            sf['samples'] = {k: c[k] for k in CATEGORIES if c[k]}
    result['_frames'] = fr
    result['_per_frame_self'] = per_frame_self
    return result


def gpu_summary(cap, fr, span, nframes):
    if not cap.gpu:
        return {}
    rows = collections.defaultdict(lambda: [0, 0.0, 0, 0.0])  # name -> rows, ms, draws, longest
    busy = idle = 0.0
    for a, b, site, draws in cap.gpu:
        start, end = max(a, span[0]), min(b, span[1])
        if end <= start:
            continue
        name = cap.sites.get(site, '?')
        r = rows[name]
        r[0] += 1
        r[1] += end - start
        r[2] += draws
        r[3] = max(r[3], end - start)
        if name == GPU_IDLE:
            idle += end - start
        else:
            busy += end - start
    calibration = [v for t, v in cap.counters.get('GPU clock calibrated', []) if span[0] <= t < span[1]]
    deviations = [v for t, v in cap.counters.get('GPU clock deviation ns', []) if span[0] <= t < span[1]]
    return {'alignment': ('calibrated' if all(calibration) else 'estimated') if calibration else 'unknown (older capture)',
            'max_deviation_ns': max(deviations) if deviations else None,
            'busy_ms_per_frame': round(busy / nframes, 3), 'idle_ms_per_frame': round(idle / nframes, 3),
            'busy_percent': round(100 * busy / (busy + idle), 1) if busy + idle else 0,
            'rows': [{'name': n, 'rows_per_frame': round(v[0] / nframes, 2), 'ms_per_frame': round(v[1] / nframes, 4),
                      'draws_per_frame': round(v[2] / nframes, 1), 'max_ms': round(v[3], 3)}
                     for n, v in sorted(rows.items(), key=lambda kv: -kv[1][1])]}


def text_report(res, top_n, slow_ms):
    out = []
    w = out.append
    w(f"Capture {res['file']}: {res['duration_s']} s, {res['events']} events")
    f = res['frames']
    if f:
        w(f"\nFrames: {f['count']}, {f['fps_avg']} fps average; frame time ms: avg {f['ms_avg']}, p50 {f['ms_p50']}, "
          f"p95 {f['ms_p95']}, p99 {f['ms_p99']}, max {f['ms_max']}; over 33 ms: {f['over_33_ms']}, over 50 ms: {f['over_50_ms']}")
    else:
        w('\nNo frames in this capture (prof_frame was not called).')
    w(f"\nTop {top_n} places by self time (ms per frame, calls per frame):")
    for t in res['top_self'][:top_n]:
        w(f"  {t['self_ms_per_frame']:8.3f}  {t['calls_per_frame']:9.1f}x  [{t['thread']}] {t['name']}")
    waits = res.get('top_waits') or []
    if waits:
        w(f"  Waiting, not counted above (the {min(5, len(waits))} longest of {len(waits)} threads and places):")
        for t in waits[:5]:
            w(f"  {t['self_ms_per_frame']:8.3f}  {t['calls_per_frame']:9.1f}x  [{t['thread']}] {t['name']}")
    g = res.get('gpu')
    if g:
        w(f"\nGPU clock alignment: {g['alignment']}" +
          (f"; maximum calibration deviation {g['max_deviation_ns']} ns" if g['max_deviation_ns'] is not None else ''))
        w(f"\nGPU: busy {g['busy_ms_per_frame']} ms per frame ({g['busy_percent']}% of the time), idle {g['idle_ms_per_frame']} ms. "
          f"Passes, dispatches and copies by GPU time (ms per frame):")
        for r in g['rows'][:top_n]:
            w(f"  {r['ms_per_frame']:8.3f}  {r['rows_per_frame']:7.2f}x  {r['draws_per_frame']:8.1f} draws  max {r['max_ms']:.2f}  {r['name']}")
    sp = res.get('sampler')
    if sp:
        w(f"\nSampler: {sp['samples']} samples, one per {sp['interval_ms']} ms per thread. Share of each thread's time:")
        w(f"  {'thread':<24}{'samples':>8}" + ''.join(f'{c:>15}' for c in CATEGORIES))
        for t in sp['threads']:
            w(f"  {t['name']:<24}{t['samples']:>8}" + ''.join(f'{t[c]:>14}%' for c in CATEGORIES))
        for key, title in (('runtime_self', 'Runtime functions by self time (the top frame was in the runtime)'),
                           ('runtime_callers_of_driver', 'Runtime functions that were calling into the driver or system'),
                           ('runtime_waits', 'Waits found by the stack, not by a zone (counted as waiting above)'),
                           ('runtime_inclusive', 'Runtime functions by inclusive time (anywhere in the stack)')):
            if sp[key]:
                w(f'\n{title}; percent of all samples, ms per frame:')
                for r in sp[key][:top_n]:
                    w(f"  {r['percent']:6.2f}%  {r['ms_per_frame']:8.3f}  {r['name']}")
        if sp['symbols_note']:
            w(f"  ({sp['symbols_note']})")
        w('  ' + COPYRIGHT_NOTE)
    for th in res['threads']:
        if not th['zones'] and not th['acc'] or th['name'] == 'gpu':
            continue
        w(f"\nThread {th['name']} (busy in zones {th['busy_ms_per_frame']} ms per frame)")
        for z in th['zones']:
            if z['incl_ms'] < 0.01:
                continue
            depth = z['path'].count('/')
            w(f"  {'  ' * depth}{z['path'].rsplit('/', 1)[-1]:<{max(34 - 2 * depth, 8)}} incl {z['incl_ms']:8.3f}  self {z['self_ms']:8.3f}"
              f"  {z['calls_per_frame']:8.1f}x  max {z['max_ms']:.2f}")
        if th['acc']:
            w('  added-up work (per frame):')
            for a in th['acc']:
                if a['incl_ms'] < 0.005:
                    continue
                depth = a['path'].count('/')
                w(f"    {'  ' * depth}{a['path'].rsplit('/', 1)[-1]:<{max(30 - 2 * depth, 8)}} incl {a['incl_ms']:8.3f}  self {a['self_ms']:8.3f}"
                  f"  {a['calls_per_frame']:9.1f}x")
    if res['counters']:
        w('\nCounters:')
        for name, c in sorted(res['counters'].items()):
            w(f"  {name:<28} min {c['min']}  avg {c['avg']}  max {c['max']}  ({c['samples']} samples)")
    slow = [s for s in res['slow_frames'] if s['ms'] > slow_ms]
    if slow:
        w(f'\nSlowest frames (over {slow_ms:g} ms), with the most self time inside each:')
        for s in slow:
            w(f"  frame {s['frame']}: {s['ms']} ms at {s['at_s']} s")
            for t in s['top']:
                w(f"      {t['ms']:7.2f} ms  [{t['thread']}] {t['name']}")
            if s.get('samples'):
                w('      samples: ' + ', '.join(f'{k} {v}' for k, v in s['samples'].items()))
    return '\n'.join(out)


def write_csv(res, path):
    fr = res['_frames']
    pfs = res['_per_frame_self']
    columns = [f"{t['thread']}:{t['name']}" for t in res['top_self'][:30] if t['self_ms_per_frame'] > 0]
    with open(path, 'w') as f:
        f.write('frame,ms,' + ','.join(c.replace(',', ';') for c in columns) + '\n')
        for i, n, start, dur in fr.items:
            row = pfs.get(i, {})
            f.write(f'{n},{dur:.3f},' + ','.join(f"{row.get(tuple(c.split(':', 1)), 0.0):.3f}" for c in columns) + '\n')


def newest():
    files = sorted(glob.glob('profile/swprof-*.swprof'), key=os.path.getmtime)
    return files[-1] if files else None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('capture', nargs='?')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--slow', type=float, default=33.4, help='list frames over this many ms (default 33.4)')
    ap.add_argument('--json', metavar='FILE')
    ap.add_argument('--csv', metavar='FILE', help='one row per frame: its time and the self time of the top places')
    ap.add_argument('--frames', metavar='FIRST:LAST', help='only these frame numbers')
    ap.add_argument('--exe', metavar='FILE', help='the runtime binary of the captured run, to name its functions in the sampler section')
    a = ap.parse_args(argv)
    path = a.capture or newest()
    if not path:
        sys.exit('no capture given and no profile/swprof-*.swprof here')
    first = last = None
    if a.frames:
        lo, _, hi = a.frames.partition(':')
        first = int(lo) if lo else None
        last = int(hi) if hi else None
    res = analyse(Capture(path), first, last, a.exe)
    print(text_report(res, a.top, a.slow))
    if a.csv:
        write_csv(res, a.csv)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({k: v for k, v in res.items() if not k.startswith('_')}, f, indent=1)


if __name__ == '__main__':
    main()
