#!/usr/bin/env python3
"""Self and inclusive time per function from build/samples.txt (runtime/sampler.c, SAVAGEWIND_SAMPLE).

Usage: profile_samples.py [samples.txt] [--top N] [--thread HANDLE] [--busy]

Addresses in the game library become f_<guest address> (its exported symbols, from `nm build/swgame.dll`), those in
the runtime the runtime's functions (`nm build/savagewind.exe`). --busy leaves out samples whose top frames are a
wait (condition variables, sleeps), so the numbers are where the CPU goes.
"""
import argparse
import bisect
import collections
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def symbols(path):
    out = subprocess.run(['nm', '--defined-only', path], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in 'Tt':
            syms.append((int(p[0], 16), p[2]))
    syms.sort()
    return syms


def image_size(path):
    # SizeOfImage from the PE optional header: addresses past it belong to the next module loaded (the GPU driver
    # often lands right after swgame.dll and was named after the library's last function)
    with open(path, 'rb') as f:
        d = f.read(4096)
    pe = int.from_bytes(d[0x3c:0x40], 'little')
    return int.from_bytes(d[pe + 24 + 56:pe + 24 + 60], 'little')


class Map:
    def __init__(self, syms, image_base, load_base):
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]
        self.delta = load_base - image_base

    def name(self, addr):
        a = addr - self.delta
        i = bisect.bisect_right(self.addrs, a) - 1
        return self.names[i] if i >= 0 else None


# A sample is a wait when one of these is on its stack (the kernel's waits, sleeps, the GPU fence wait), and a
# wait for a core when core_acquire is (runnable, but the emulated core is taken).
WAITS = ('kwait', 'block_current', 'pthread_cond_wait', 'pthread_cond_timedwait', 'plat_sleep_until_ns',
         'kernel_sleep_until_ns', 'nanosleep', 'vkWaitSemaphores', 'vk_wait_seq')
CORE_WAIT = 'core_acquire'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples', nargs='?', default=os.path.join(ROOT, 'build', 'samples.txt'))
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', type=lambda v: int(v, 0))
    ap.add_argument('--busy', action='store_true')
    a = ap.parse_args()
    bases, threads, samples = {}, {}, []
    for line in open(a.samples):
        p = line.split()
        if p[0] == 'M':
            bases[p[1]] = int(p[2], 16)
        elif p[0] == 'T':
            threads[int(p[1])] = p[2]
        elif p[0] == 'S':
            samples.append((int(p[1]), [int(x, 16) for x in p[2:]]))
    exe = Map(symbols(os.path.join(ROOT, 'build', 'savagewind.exe')), 0x140000000, bases.get('exe', 0x140000000))
    dll = Map(symbols(os.path.join(ROOT, 'build', 'swgame.dll')), 0x180000000, bases.get('swgame', 0x180000000))
    dll_lo = bases.get('swgame', 0)
    dll_size = image_size(os.path.join(ROOT, 'build', 'swgame.dll'))
    exe_size = image_size(os.path.join(ROOT, 'build', 'savagewind.exe'))

    def name(addr):
        if dll_lo and dll_lo <= addr < dll_lo + dll_size:
            return dll.name(addr) or '?swgame'
        if bases.get('exe', 0) <= addr < bases.get('exe', 0) + exe_size:
            return exe.name(addr) or '?exe'
        return '?%x' % (addr >> 20 << 20)  # system libraries: by megabyte

    selfc, incl, per_thread, kinds = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
    total = 0
    for th, stack in samples:
        if a.thread is not None and th != a.thread:
            continue
        names = [name(x) for x in stack]
        kind = 'core' if any(n == CORE_WAIT for n in names) else 'wait' if any(n in WAITS for n in names) else 'busy'
        kinds[kind] += 1
        if a.busy and kind != 'busy':
            continue
        total += 1
        per_thread[th] += 1
        if names:
            selfc[names[0]] += 1
        for n in set(names):
            incl[n] += 1
    if not total:
        print('no samples')
        return
    n_all = sum(kinds.values())
    print('%d samples: busy %.1f%%, waiting %.1f%%, waiting for a core %.1f%%' % (
        n_all, 100.0 * kinds['busy'] / n_all, 100.0 * kinds['wait'] / n_all, 100.0 * kinds['core'] / n_all))
    print('%d counted' % total)
    print('\nby thread:')
    for th, c in per_thread.most_common(12):
        print('  %5.1f%%  handle %#x entry %s' % (100.0 * c / total, th, threads.get(th, '?')))
    print('\nself:')
    for n, c in selfc.most_common(a.top):
        print('  %5.1f%%  %s' % (100.0 * c / total, n))
    print('\ninclusive:')
    for n, c in incl.most_common(a.top):
        print('  %5.1f%%  %s' % (100.0 * c / total, n))


if __name__ == '__main__':
    main()
