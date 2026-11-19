#!/usr/bin/env python3
"""Where the game's memory goes (Windows): boots one place as tools/perf_routes.py does, lets it settle, then walks the
process's address space from outside (VirtualQueryEx) and adds up what is committed, and how much of that is in the
working set (QueryWorkingSetEx), by kind:

  guest heap / guest modules / guest stacks / guest other   the guest window (runtime/rt.h), by region
  image <name>                                              the exe, the game code library and the DLLs
  mapped                                                    file and section mappings
  private <size class>                                      everything else, by the size of the allocation

Committed is what counts against RAM + page file; in the working set is what occupies RAM now. Memory a driver keeps
for video memory does not show as a region: it is the difference between the process's private bytes and the walk.

    python tools/ram_map.py --save <scratch save folder> --ini <settings> --out <dir> [--place plateau]
        [--size 1280x720] [--settle 40] [--env VAR=value ...] [--tag name] [--top 25]

Prints the table and writes <out>/<tag>.txt. The game runs hidden on its own port (7791) and is closed at the end.
"""
import argparse
import collections
import ctypes
import ctypes.wintypes as W
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perf_routes  # noqa: E402

MEM_COMMIT, MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x1000, 0x20000, 0x40000, 0x1000000
PAGE = 4096
GUEST = [  # (runtime/rt.h, runtime/kernel.h)
    (0x7000000000, 0x7080000000, 'guest modules'),
    (0x7080000000, 0x7100000000, 'guest hle + tls'),
    (0x7100000000, 0x7200000000, 'guest alias'),
    (0x7200000000, 0x7240000000, 'guest stacks'),
    (0x7240000000, 0x7320000000, 'guest heap'),
]


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64), ('AllocationProtect', W.DWORD),
                ('PartitionId', W.WORD), ('RegionSize', ctypes.c_uint64), ('State', W.DWORD), ('Protect', W.DWORD),
                ('Type', W.DWORD)]


class WSX(ctypes.Structure):
    _fields_ = [('VirtualAddress', ctypes.c_uint64), ('Attributes', ctypes.c_uint64)]


def size_class(n):
    for lim, name in ((1 << 20, 'under 1 MB'), (16 << 20, '1-16 MB'), (128 << 20, '16-128 MB'), (1 << 30, '128 MB-1 GB')):
        if n < lim:
            return name
    return '1 GB and up'


def walk(handle, top):
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    psapi = ctypes.WinDLL('psapi', use_last_error=True)
    k32.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
    psapi.QueryWorkingSetEx.argtypes = [W.HANDLE, ctypes.c_void_p, W.DWORD]
    psapi.GetMappedFileNameW.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.c_wchar_p, W.DWORD]
    regions = []
    addr, mbi = 0, MBI()
    while k32.VirtualQueryEx(handle, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if mbi.State == MEM_COMMIT:
            regions.append((mbi.BaseAddress, mbi.RegionSize, mbi.Type, mbi.AllocationBase))
        nxt = mbi.BaseAddress + mbi.RegionSize
        if nxt <= addr:
            break
        addr = nxt
    alloc_size = collections.Counter()
    for base, size, typ, alloc in regions:
        alloc_size[alloc] += size
    commit, resident = collections.Counter(), collections.Counter()
    by_alloc = collections.defaultdict(lambda: [0, 0, ''])
    names = {}
    batch = 65536
    buf = (WSX * batch)()
    for base, size, typ, alloc in regions:
        kind = next((n for lo, hi, n in GUEST if lo <= base < hi), None)
        if not kind:
            if typ == MEM_IMAGE:
                if alloc not in names:
                    s = ctypes.create_unicode_buffer(520)
                    names[alloc] = os.path.basename(s.value) if psapi.GetMappedFileNameW(handle, alloc, s, 520) else '?'
                kind = 'image ' + names[alloc]
            elif typ == MEM_MAPPED:
                kind = 'mapped'
            else:
                kind = 'private ' + size_class(alloc_size[alloc])
        res = 0
        for off in range(0, size, batch * PAGE):
            n = min(batch, (size - off) // PAGE)
            for i in range(n):
                buf[i].VirtualAddress = base + off + i * PAGE
            if psapi.QueryWorkingSetEx(handle, buf, n * ctypes.sizeof(WSX)):
                res += sum(1 for i in range(n) if buf[i].Attributes & 1) * PAGE
        commit[kind] += size
        resident[kind] += res
        a = by_alloc[alloc]
        a[0] += size
        a[1] += res
        a[2] = kind
    hist = collections.Counter()  # the guest heap's pages in RAM, per 64 MB of it
    lo, hi = GUEST[-1][0], GUEST[-1][1]
    for base, size, typ, alloc in regions:
        a, e = max(base, lo), min(base + size, hi)
        for off in range(0, max(e - a, 0), batch * PAGE):
            n = min(batch, (e - a - off) // PAGE)
            for i in range(n):
                buf[i].VirtualAddress = a + off + i * PAGE
            if psapi.QueryWorkingSetEx(handle, buf, n * ctypes.sizeof(WSX)):
                for i in range(n):
                    if buf[i].Attributes & 1:
                        hist[(a + off + i * PAGE - lo) >> 26] += PAGE
    lines = ['{:34} {:>10} {:>12}'.format('kind', 'commit MB', 'in RAM MB')]
    images = [(k, v) for k, v in commit.items() if k.startswith('image ')]
    small = sum(v for k, v in images if v < (8 << 20))
    small_res = sum(resident[k] for k, v in images if v < (8 << 20))
    for k, v in sorted(commit.items(), key=lambda x: -x[1]):
        if k.startswith('image ') and v < (8 << 20):
            continue
        lines.append('{:34} {:10.0f} {:12.0f}'.format(k, v / 2**20, resident[k] / 2**20))
    lines.append('{:34} {:10.0f} {:12.0f}'.format('image (others under 8 MB)', small / 2**20, small_res / 2**20))
    lines.append('{:34} {:10.0f} {:12.0f}'.format('TOTAL', sum(commit.values()) / 2**20, sum(resident.values()) / 2**20))
    lines.append('')
    lines.append('largest allocations outside the guest window and the images:')
    rows = [(b, a) for b, a in by_alloc.items() if a[2].startswith(('private', 'mapped'))]
    for b, a in sorted(rows, key=lambda x: -x[1][0])[:top]:
        lines.append('  {:#014x} {:22} {:8.0f} MB commit {:8.0f} MB in RAM'.format(b, a[2], a[0] / 2**20, a[1] / 2**20))
    lines.append('')
    lines.append('guest heap in RAM, MB per 64 MB from its start:')
    top_bucket = max(hist) if hist else -1
    for row in range(0, top_bucket + 1, 16):
        lines.append('  {:5d} MB: '.format(row * 64) + ' '.join('{:2d}'.format(hist[b] >> 20) for b in range(row, min(row + 16, top_bucket + 1))))
    return lines


def perf_size(n):
    return '{} MB'.format(n >> 20) if n >= 1 << 20 else '{} KB'.format(n >> 10) if n >= 1024 else '{} B'.format(n)


def limit_commit(handle, mb):
    """A job object with JOB_OBJECT_LIMIT_PROCESS_MEMORY: allocations past the limit fail, as with RAM + page file full."""
    class Basic(ctypes.Structure):
        _fields_ = [('PerProcessUserTimeLimit', ctypes.c_int64), ('PerJobUserTimeLimit', ctypes.c_int64),
                    ('LimitFlags', W.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                    ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', W.DWORD),
                    ('Affinity', ctypes.c_size_t), ('PriorityClass', W.DWORD), ('SchedulingClass', W.DWORD)]

    class Extended(ctypes.Structure):
        _fields_ = [('Basic', Basic), ('IoInfo', ctypes.c_uint64 * 6), ('ProcessMemoryLimit', ctypes.c_size_t),
                    ('JobMemoryLimit', ctypes.c_size_t), ('PeakProcessMemoryUsed', ctypes.c_size_t),
                    ('PeakJobMemoryUsed', ctypes.c_size_t)]
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.CreateJobObjectW.restype = W.HANDLE
    k32.SetInformationJobObject.argtypes = [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD]
    k32.AssignProcessToJobObject.argtypes = [W.HANDLE, W.HANDLE]
    job = k32.CreateJobObjectW(None, None)
    info = Extended()
    info.Basic.LimitFlags = 0x100
    info.ProcessMemoryLimit = mb << 20
    if not k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)) or             not k32.AssignProcessToJobObject(job, handle):
        raise OSError('job object: error {}'.format(ctypes.get_last_error()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree', default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument('--save', required=True)
    ap.add_argument('--ini')
    ap.add_argument('--out', required=True)
    ap.add_argument('--place', default='plateau')
    ap.add_argument('--size', default='1280x720')
    ap.add_argument('--settle', type=float, default=40)
    ap.add_argument('--boot', type=float, default=240)
    ap.add_argument('--port', type=int, default=7791)
    ap.add_argument('--env', nargs='*', default=[])
    ap.add_argument('--tag', default='map')
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--exe')
    ap.add_argument('--commit-mb', type=int, default=0, help='limit the process to this much committed memory (a job '
                    'object): what a PC with that much RAM + page file left for the game allows')
    ap.add_argument('--tour', nargs='*', default=[], help='teleport to these places first, e.g. "tower 6" "village 4" '
                    '(docs/teleport-places.md); memory is printed after each')
    args = ap.parse_args()
    if os.name != 'nt':
        sys.exit('ram_map.py walks a Windows process; on Linux read /proc/<pid>/smaps')
    os.environ['PATH'] = r'C:\msys64\ucrt64\bin;' + os.environ.get('PATH', '')
    args.ini = args.ini or os.path.join(args.tree, 'savagewind.ini')
    for k, v in (('potato', False), ('visible', False), ('fullscreen', False), ('data_root', None), ('cache_reset', None),
                 ('vram_mb', 0), ('ram_mb', 0)):
        setattr(args, k, v)
    run_dir = os.path.join(args.out, args.tag)
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir)
    proc, log = perf_routes.launch(args, run_dir, args.size, 0, args.env + ['SAVAGEWIND_TEST_WARP=' + args.place])
    game = None
    tour = []
    try:
        if args.commit_mb:
            limit_commit(int(proc._handle), args.commit_mb)
        game = perf_routes.Game(args.port)
        perf_routes.wait_world(game, proc, args.boot)
        time.sleep(args.settle)
        for stop in args.tour:
            game.cmd('goto', *stop.split(), ok=False)
            st = game.poll(['teleport', 'status'], ('done', 'failed'), 90)
            time.sleep(8)
            m = game.cmd('memstats')
            tour.append('after {:14} ({}): private {:.0f} MB, working set {:.0f} MB, GPU device-local {:.0f} MB'.format(
                stop, st, m.get('private_mb', -1), m.get('rss_mb', -1), m.get('gpu_local_mb', -1)))
            print(tour[-1], flush=True)
        stats = game.cmd('memstats')
        heaps = game.cmd('heaps', ok=False, timeout=120)
        lines = ['{} at {} {}, {} s after the world loaded; env {}'.format(args.tag, args.place, args.size, int(args.settle),
                                                                          ' '.join(args.env) or '-'),
                 'process: private {:.0f} MB, working set {:.0f} MB; GPU device-local {:.0f} MB, host-visible {:.0f} MB'
                 .format(stats.get('private_mb', -1), stats.get('rss_mb', -1), stats.get('gpu_local_mb', -1),
                         stats.get('gpu_host_mb', -1)), ''] + tour + ([''] if tour else [])
        if 'crt_busy_mb' in heaps:
            lines.append('C heap (malloc): {:.0f} MB busy, {:.0f} MB committed; other heaps (drivers, system): {:.0f} MB '
                         'busy, {:.0f} MB committed'.format(heaps['crt_busy_mb'], heaps['crt_commit_mb'],
                                                            heaps['other_busy_mb'], heaps['other_commit_mb']))
            cls = sorted((int(k.split('_')[1].split('.')[0]), v) for k, v in heaps.items() if k.startswith('crt.le_') and k.endswith('.mb'))
            lines.append('C heap busy MB by block size (up to): ' + ', '.join(
                '{}: {:.0f} ({:.0f})'.format(perf_size(n), v, heaps['crt.le_{}.count'.format(n)]) for n, v in cls if v >= 4))
            lines.append('C heap blocks of 2 KB and more by their first 4 bytes: ' + ', '.join(
                '{} up to {}: {:.0f} MB ({:.0f})'.format(k.split('.')[1], perf_size(int(k.split('_')[1].split('.')[0])), v,
                                                         heaps[k[:-2] + 'count'])
                for k, v in sorted(heaps.items(), key=lambda x: -x[1] if x[0].startswith('sig.') and x[0].endswith('.mb') else 0)
                if k.startswith('sig.') and k.endswith('.mb')))
            lines.append('')
        lines += walk(int(proc._handle), args.top)
    finally:
        perf_routes.stop_game(game, proc, log)
    text = '\n'.join(lines)
    sys.stdout.reconfigure(errors='replace')  # (a DLL's name outside the console's code page)
    print(text)
    with open(os.path.join(args.out, args.tag + '.txt'), 'w', encoding='utf-8') as f:
        f.write(text + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
