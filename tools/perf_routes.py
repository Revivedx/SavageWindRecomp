#!/usr/bin/env python3
"""Fixed-view performance routes: boot the game, visit fixed places, measure each, write CSV + markdown.

Standard library only (Python 3.8+). Drives the game through the control socket (docs/CONTROL.md, tools/swctl.py).
Docs: docs/PERF-ROUTES.md.

  perf_routes.py run   --tree <worktree> --save <save folder> --out <dir> [--size 2560x1440 ...] [--runs 3] ...
  perf_routes.py table <dir> [<dir>...]       markdown table (median and spread over runs) from results.csv files

Every boot plays the same routes in the same order: fly mode on, teleport, camera, fixed hour (frozen), settle,
sample. The game runs hidden (SAVAGEWIND_HIDDEN=1), uncapped, vsync off, frame generation off, perf overlay off; the
other settings are the player's savagewind.ini (--ini).

Linux (the Steam Deck): run it on the machine itself, as a user systemd unit so it outlives the SSH session, with --exe
<AppImage> --data-root <its own data root>; the thread shares then come from CPU time (no stack sampler there).
"""
import argparse
import collections
import csv
import ctypes
import datetime
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time

try:
    from . import profile_samples, swctl
except ImportError:
    import profile_samples
    import swctl

# name, warp place (SAVAGEWIND_TEST_WARP: the game's own start position there, runtime/services/test_warp.c), camera
# yaw and pitch (degrees; yaw 0 looks along +z, south; 90 along +x, east; pitch negative looks down), hour. Each route
# is its own boot: Continue loads straight into the place, so no long teleport has to stream a far area in (a far
# `teleport` leaves the game waiting on its loading spinner). hop: a short teleport from there (the area is loaded)
# with fly mode on, so Link hovers at a better viewpoint. Views checked by eye with `shot` (docs/PERF-ROUTES.md).
ROUTES = [
    ('plateau',  'plateau',   150.0, -3.0, 10.0, None),
    ('kakariko', 'kakariko',  180.0, -10.0, 10.0, None),
    ('field',    'stable',    180.0, -3.0, 10.0, None),
    ('rito',     'rito',      180.0, -8.0, 10.0, (-3610.0, 300.0, -1730.0)),
    ('castle',   'castle',    180.0, 0.0, 10.0, (-254.0, 260.0, -400.0)),
    ('hateno',   'hateno',     90.0, -8.0, 10.0, None),
]

CONTINUE = '150:A:2,170:A:2,190:A:2,210:A:2'  # (SAVAGEWIND_SCRIPT_CLOCK=1: 30ths of a second) title, Continue

VK_LINE = re.compile(r'^\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms \((.*)\)\s*$')


class Game:
    def __init__(self, port):
        self.port = port
        self.conn = swctl.connect('127.0.0.1', port, 5, 120)

    def cmd(self, *words, ok=True, timeout=10):
        reply = swctl.parse_reply(swctl.exchange(self.conn, swctl.request_line([str(w) for w in words]), timeout), words[0])
        if ok and not reply['ok']:
            raise RuntimeError('{}: {}'.format(' '.join(map(str, words)), reply['error']))
        return reply.get('data') if reply['ok'] else reply

    def poll(self, words, done, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            data = self.cmd(*words)
            st = data.get('state', data.get('status'))
            if st in done:
                return st
            time.sleep(0.2)
        return 'timeout'

    def close(self):
        try:
            self.conn.close()
        except OSError:
            pass


def amdgpu_dir():
    """The amdgpu device in sysfs (the Steam Deck's APU), or None."""
    import glob
    for d in sorted(glob.glob('/sys/class/drm/card[0-9]/device')):
        if os.path.exists(os.path.join(d, 'gpu_busy_percent')):
            return d
    return None


def amdgpu_read(d):
    """GPU clock (MHz: hwmon freq1_input, else the starred line of pp_dpm_sclk), load (%), power (W: the APU's, hwmon
    power1_average: on the Steam Deck CPU and GPU share it, capped at 15 W), mean CPU clock (MHz)."""
    import glob
    sclk = glob.glob(os.path.join(d, 'hwmon', 'hwmon*', 'freq1_input'))
    if sclk:
        with open(sclk[0]) as f:
            clock = float(f.read()) / 1e6
    else:
        with open(os.path.join(d, 'pp_dpm_sclk')) as f:
            clock = next((float(re.search(r'(\d+)\s*Mhz', l, re.I).group(1)) for l in f if '*' in l), -1.0)
    with open(os.path.join(d, 'gpu_busy_percent')) as f:
        load = float(f.read())
    power = -1.0
    for p in sorted(glob.glob(os.path.join(d, 'hwmon', 'hwmon*', 'power1_*'))):
        if p.endswith(('_average', '_input')):
            with open(p) as f:
                power = float(f.read()) / 1e6
            break
    cpu = []
    for p in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq'):
        with open(p) as f:
            cpu.append(float(f.read()) / 1e3)
    return [clock, load, power, statistics.mean(cpu) if cpu else -1.0]


def gpu_sampler(stop, out):
    """GPU clock, load and power once a second (nvidia-smi, or amdgpu's sysfs files): a hidden run with background
    clocks shows here."""
    exe = shutil.which('nvidia-smi')
    amd = None if exe else amdgpu_dir()
    while (exe or amd) and not stop.wait(1.0):
        try:
            if amd:
                out.append(amdgpu_read(amd))
                continue
            line = subprocess.run([exe, '--query-gpu=clocks.gr,utilization.gpu,power.draw', '--format=csv,noheader,nounits'],
                                  capture_output=True, text=True, timeout=5).stdout.strip().splitlines()[0]
            out.append([float(v) for v in line.split(',')])
        except (OSError, ValueError, IndexError, AttributeError, subprocess.SubprocessError):
            pass


def linux_games():
    """(pid, session id) of every running savagewind process (the AppImage's game binary or a plain build)."""
    found = []
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            with open('/proc/{}/comm'.format(pid)) as f:
                if f.read().strip() != 'savagewind':
                    continue
            found.append((int(pid), os.getsid(int(pid))))
        except OSError:
            pass
    return found


MY_SESSIONS = set()  # (Linux) the sessions of the games this script started


def others(tree):
    """savagewind.exe processes not started from this tree (another session's run shares the GPU). Linux: savagewind
    processes this script did not start (the player's game)."""
    if os.name != 'nt':
        return ['{} (session {})'.format(p, s) for p, s in linux_games() if s not in MY_SESSIONS]
    ps = subprocess.run(['powershell', '-NoProfile', '-Command',
                         "Get-CimInstance Win32_Process -Filter \"Name='savagewind.exe'\" | "
                         "ForEach-Object { \"$($_.ProcessId) $($_.ExecutablePath)\" }"], capture_output=True, text=True)
    mine = os.path.normcase(os.path.abspath(tree))
    return [l for l in ps.stdout.splitlines() if l.strip() and mine not in os.path.normcase(l)]


def game_cwd(args):
    """Where the game works (its data root: the tree in a dev build): shots and traces land under its build/."""
    return args.data_root or args.tree


def game_log(args, run_dir):
    """The game's log: its stdout (log.txt in the run's folder), or with a data root of its own on Linux, where the
    game sends stdout and stderr to savagewind.log there when they are not a terminal (runtime/platform/linux.c)."""
    if args.data_root and os.name != 'nt':
        return os.path.join(args.data_root, 'savagewind.log')
    return os.path.join(run_dir, 'log.txt')


def reset_cache(args, run_dir):
    """--cache-reset <dir>: the data root's shader caches put back as they were after setup (a player's first boot):
    <dir>/cache becomes the data root's cache/, and the driver's on-disk shader cache (Mesa) starts as <dir>/mesa (as
    setup's precompile left it) or empty."""
    if not args.cache_reset:
        return {}
    dst = os.path.join(game_cwd(args), 'cache')
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(os.path.join(args.cache_reset, 'cache'), dst, symlinks=True)
    mesa = os.path.join(run_dir, 'mesa-cache')
    if os.path.isdir(os.path.join(args.cache_reset, 'mesa')):
        shutil.copytree(os.path.join(args.cache_reset, 'mesa'), mesa, symlinks=True)
    os.makedirs(mesa, exist_ok=True)
    return {'MESA_SHADER_CACHE_DIR': mesa}


def launch(args, run_dir, size, affinity, extra_env):
    save = os.path.join(run_dir, 'save')
    shutil.copytree(args.save, save)
    ini = []
    with open(args.ini, encoding='utf-8') as f:
        for line in f:
            key = line.split('=', 1)[0].strip()
            forced = {'window_size': size, 'frame_rate': '0', 'vsync': 'off', 'perf_overlay': 'off',
                      'frame_generation': '0', 'fullscreen': '1' if args.fullscreen else '0'}
            ini.append('{} = {}\n'.format(key, forced[key]) if key in forced else line)
    ini_path = os.path.join(run_dir, 'settings.ini')
    with open(ini_path, 'w', encoding='utf-8') as f:
        f.writelines(ini)
    env = dict(os.environ, SAVAGEWIND_NO_LAUNCHER='1', SAVAGEWIND_SAVE_DIR=save,
               SAVAGEWIND_SETTINGS=ini_path, SAVAGEWIND_CONTROL=str(args.port), SAVAGEWIND_VK_PROF='2',
               SAVAGEWIND_FPS_INTERVAL='1', SAVAGEWIND_MAX_INTERVAL='2', SAVAGEWIND_SCRIPT_CLOCK='1',
               SAVAGEWIND_INPUT=CONTINUE, SAVAGEWIND_TEST_WEATHER='0', SAVAGEWIND_PLACE_LOG='0', SAVAGEWIND_HITCH_MS='34')
    if not args.visible:
        env['SAVAGEWIND_HIDDEN'] = '1'
    if args.data_root:
        env['SAVAGEWIND_DATA_ROOT'] = args.data_root
        os.makedirs(os.path.join(args.data_root, 'build'), exist_ok=True)
    env.update(reset_cache(args, run_dir))
    for kv in extra_env:
        k, v = kv.split('=', 1)
        env[k] = v
    exe = args.exe or os.path.join(args.tree, 'build', 'savagewind.exe' if os.name == 'nt' else 'savagewind')
    log = open(os.path.join(run_dir, 'log.txt'), 'wb')
    flags = 0x4 if (os.name == 'nt' and affinity) else 0  # CREATE_SUSPENDED: the mask is in place before any thread runs
    argv = [exe] if args.data_root else [exe, 'extracted']  # (a data root of its own: its extracted/, as a player's)
    proc = subprocess.Popen(argv, cwd=args.tree, env=env, stdout=log, stderr=subprocess.STDOUT,
                            creationflags=flags, start_new_session=os.name != 'nt')  # (Linux: its own group, killed whole)
    if os.name != 'nt':
        MY_SESSIONS.add(proc.pid)
    if flags:
        k32 = ctypes.WinDLL('kernel32')
        k32.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        if not k32.SetProcessAffinityMask(int(proc._handle), affinity):
            proc.kill()
            raise OSError('SetProcessAffinityMask failed')
        ctypes.WinDLL('ntdll').NtResumeProcess(ctypes.c_void_p(int(proc._handle)))
    elif affinity:
        os.sched_setaffinity(proc.pid, [i for i in range(64) if affinity >> i & 1])
    return proc, log


def wait_world(game, proc, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if proc.poll() is not None:
            raise RuntimeError('the game exited during boot (exit code {:#x})'.format(proc.returncode & 0xffffffff))
        try:
            r = game.cmd('pos', ok=False)
        except OSError:  # (a boot can sit on one frame for more than the command timeout: a new connection, again)
            game.close()
            game.conn = swctl.connect('127.0.0.1', game.port, 5, 30)
            continue
        if 'error' not in r:
            return
        time.sleep(1)
    raise RuntimeError('no world after {} s'.format(seconds))


def vk_lines(log_path, start, end):
    with open(log_path, 'rb') as f:
        f.seek(start)
        text = f.read(end - start).decode('utf-8', 'replace')
    rows, hitches = [], 0
    for line in text.splitlines():
        hitches += line.startswith('[hitch]')
        m = VK_LINE.match(line)
        if not m:
            continue
        stats = m.group(5)
        num = lambda pat: float(re.search(pat, stats).group(1)) if re.search(pat, stats) else 0.0
        rows.append({'frame': int(m.group(2)), 'gpu': num(r'\| gpu (\d+)'), 'exec': num(r'cpu exec (\d+)'),
                     'submit': num(r'\) submit (\d+) ms'), 'wait': num(r'wait (\d+)ms'), 'compile': num(r'compile (\d+)'),
                     'tr_ms': num(r'translate (\d+)'), 'pipe_ms': num(r'pipeline (\d+)'), 'draws': num(r'^(\d+) draws')})
    return rows, hitches


def gpuprof(log_path, start, top):
    """Top passes of the captured frame: rows with the same label summed."""
    with open(log_path, 'rb') as f:
        f.seek(start)
        text = f.read().decode('utf-8', 'replace')
    total, rows = None, {}
    for line in text.splitlines():
        m = re.match(r'^\[gpuprof\] \d+ marks, ([\d.]+) ms', line)
        if m:
            if total is not None:
                break
            total = float(m.group(1))
            continue
        m = re.match(r'^\[gpuprof\]\s+([\d.]+) ms\s+[\d.]+%\s+(.*?)(?:, (\d+) draws)?$', line)
        if m and total is not None:
            label = m.group(2)
            ms, draws = rows.get(label, (0.0, 0))
            rows[label] = (ms + float(m.group(1)), draws + int(m.group(3) or 0))
    best = sorted(rows.items(), key=lambda kv: -kv[1][0])[:top]
    return total, '; '.join('{:.2f} ms {}{}'.format(ms, label, ' ({} draws)'.format(d) if d else '')
                            for label, (ms, d) in best)


# Stack samples (runtime/sampler.c, the `sample` command) sorted per thread: the game's main thread (the first guest
# thread, handle 0x101) and the NVN queue thread (render: translation and recording, handle 0xf000). A main-thread
# sample in queue_wait waits for the render thread; in vk_wait_seq / renderer_sync for the GPU; in any other wait for
# other guest threads, a sleep or a free core. A render-thread sample whose first known frame is queue_thread is idle
# (no command list to run). CPU time per thread (`threads`) cannot tell this: some threads spin while they wait.
MAIN_HANDLE, RENDER_HANDLE = 0x101, 0xf000
GPU_WAITS = ('vk_wait_seq', 'vkWaitSemaphores', 'renderer_sync')
OTHER_WAITS = tuple(profile_samples.WAITS) + ('core_acquire', 'kernel_preempt')
_maps = {}


def sample_shares(tree, path):
    if not _maps:
        b = lambda f: os.path.join(tree, 'build', f)
        _maps['exe'] = (profile_samples.symbols(b('savagewind.exe')), 0x140000000, profile_samples.image_size(b('savagewind.exe')))
        _maps['swgame'] = (profile_samples.symbols(b('swgame.dll')), 0x180000000, profile_samples.image_size(b('swgame.dll')))
    bases, maps = {}, {}
    kinds = {MAIN_HANDLE: collections.Counter(), RENDER_HANDLE: collections.Counter()}
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == 'M' and p[1] in _maps:
                syms, image, size = _maps[p[1]]
                bases[p[1]] = (int(p[2], 16), size, profile_samples.Map(syms, image, int(p[2], 16)))
                continue
            if p[0] != 'S' or int(p[1]) not in kinds:
                continue
            names = []
            for x in p[2:]:
                a = int(x, 16)
                for lo, size, m in bases.values():
                    if lo <= a < lo + size:
                        names.append(m.name(a) or '?')
            if int(p[1]) == RENDER_HANDLE:
                kind = ('idle' if names and names[0] == 'queue_thread' else
                        'gpu' if any(n in GPU_WAITS for n in names) else 'busy')
            else:
                kind = ('render' if 'queue_wait' in names else 'gpu' if any(n in GPU_WAITS for n in names) else
                        'other' if any(n in OTHER_WAITS for n in names) else 'busy')
            kinds[int(p[1])][kind] += 1
    share = lambda th, k: kinds[th][k] / max(1, sum(kinds[th].values()))
    return {'main_busy': share(MAIN_HANDLE, 'busy'), 'main_wait_render': share(MAIN_HANDLE, 'render'),
            'main_wait_gpu': share(MAIN_HANDLE, 'gpu'), 'main_wait_other': share(MAIN_HANDLE, 'other'),
            'render_busy': share(RENDER_HANDLE, 'busy'), 'render_wait_gpu': share(RENDER_HANDLE, 'gpu'),
            'render_idle': share(RENDER_HANDLE, 'idle')}


def mem_summary(polls, first, last):
    """memstats polled once a second through the window: peak and median of the process and GPU memory; the texture
    split at the end; ASTC decode CPU time during the window."""
    res = {}
    for key, out in (('rss_mb', 'ram'), ('gpu_local_mb', 'gpu_local'), ('gpu_host_mb', 'gpu_host'),
                     ('sys_used_mb', 'sys_used')):
        vals = [p[key] for p in polls if key in p]
        res[out + '_peak_mb'] = max(vals) if vals else -1
        res[out + '_med_mb'] = statistics.median(vals) if vals else -1
    res['ram_hwm_mb'] = last.get('rss_peak_mb', -1)
    for k in ('astc_native', 'astc_rgba8', 'astc_src', 'bc', 'rt', 'other'):
        res['tex_{}_mb'.format(k)] = last.get('tex_{}_mb'.format(k), -1)
    res['astc_ms'] = last.get('astc_decode_ms', 0) - first.get('astc_decode_ms', 0) if first and last else -1
    heaps = []
    for k, v in sorted(last.items()):
        m = re.match(r'heap(\d+)_(local|host)_used_mb$', k)
        if m:
            heaps.append('{}{} {:.0f}/{:.0f}'.format(m.group(2), m.group(1), v, last.get('heap{}_budget_mb'.format(m.group(1)), -1)))
    res['heaps'] = ', '.join(heaps)
    return res


# A hitch: a frame over HITCH_X times the window's median. Its cause: the largest of the stalls the frame trace times
# (a draw waiting for its pipeline, ASTC decoding, waiting for the GPU beyond the window's median wait, file reads,
# vkAllocateMemory) when it explains a quarter of the frame's excess or more; else what else happened in it (pipelines
# built on the workers, an upload of 4 MB or more, page faults over 4x the median); else unknown.
HITCH_X = 2.0
STALLS = (('build_wait', 'build_wait_ms'), ('astc', 'astc_ms'), ('gpu_wait', 'gpu_wait_excess'), ('file_read', 'file_ms'),
          ('vram_alloc', 'valloc_ms'), ('present', 'present_excess'))
# No stall: the thread whose CPU time in the frame went up the most over its median (game logic, translation, recording)
SPIKES = (('main_cpu', 'main_excess'), ('render_cpu', 'render_excess'), ('recorder_cpu', 'recorder_excess'))


def hitch_analysis(trace_path, out_path):
    with open(trace_path, encoding='utf-8') as f:
        rows = [{k: float(v) for k, v in r.items()} for r in csv.DictReader(f)]
    if len(rows) < 10:
        return {'hitches': -1, 'hitch_causes': '', 'hitch_ms': -1}
    rows = rows[1:]  # (the first frame began before the trace)
    med = statistics.median(r['frame_ms'] for r in rows)
    med_wait = statistics.median(r['gpu_wait_ms'] for r in rows)
    med_faults = statistics.median(r['faults'] for r in rows)
    medk = {k: statistics.median(r.get(k, 0.0) for r in rows) for k in ('present_ms', 'main_ms', 'render_ms', 'recorder_ms')}
    hits = []
    for i, r in enumerate(rows):
        if r['frame_ms'] <= HITCH_X * med:
            continue
        r = dict(r, gpu_wait_excess=max(0.0, r['gpu_wait_ms'] - med_wait), index=i)
        for k in ('present', 'main', 'render', 'recorder'):
            r[k + '_excess'] = max(0.0, r.get(k + '_ms', 0.0) - medk[k + '_ms'])
        excess = r['frame_ms'] - med
        name, ms = max(((n, r[k]) for n, k in STALLS), key=lambda x: x[1])
        spike, sms = max(((n, r[k]) for n, k in SPIKES), key=lambda x: x[1])
        if ms >= 0.25 * excess:
            cause = name
        elif r['build_ms'] >= 0.25 * excess:  # (synchronous shaders, the default: built on the render thread itself)
            cause = 'pipeline_build'
        elif sms >= 0.25 * excess:
            cause = spike
        elif r['builds'] > 0:
            cause = 'builds'
        elif r['upload_mb'] >= 4:
            cause = 'upload'
        elif r['faults'] > 4 * max(1.0, med_faults):
            cause = 'faults'
        else:
            cause = 'unknown'
        r['cause'] = cause
        hits.append(r)
    cols = ['index', 'end_ms', 'frame_ms', 'cause', 'builds', 'build_ms', 'build_waits', 'build_wait_ms', 'upload_mb',
            'tex_mb', 'astc_ms', 'astc_mb', 'gpu_wait_ms', 'gpu_wait_excess', 'file_reads', 'file_mb', 'file_ms',
            'valloc_ms', 'present_ms', 'faults', 'hard_faults', 'main_ms', 'render_ms', 'recorder_ms', 'main_excess',
            'render_excess', 'recorder_excess']
    with open(out_path, 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, cols, extrasaction='ignore')
        w.writeheader()
        for h in hits:
            w.writerow({k: round(v, 3) if isinstance(v, float) else v for k, v in h.items()})
    causes = collections.Counter(h['cause'] for h in hits)
    tot = lambda k: sum(r[k] for r in rows)
    return {'hitches': len(hits), 'hitch_causes': ' '.join('{}:{}'.format(k, v) for k, v in causes.most_common()),
            'hitch_ms': sum(h['frame_ms'] for h in hits), 'median_ms': med,
            'win_builds': int(tot('builds')), 'win_build_wait_ms': tot('build_wait_ms'), 'win_upload_mb': tot('upload_mb'),
            'win_file_mb': tot('file_mb'), 'win_faults': int(tot('faults'))}


def measure(game, args, log_path, route, run_dir, tag):
    name, place, yaw, pitch, hour, hop = route
    if hop:
        game.cmd('teleport', *hop)
        if game.poll(['teleport', 'status'], ('done', 'failed', 'idle'), 30) != 'done':
            raise RuntimeError('teleport to {} failed'.format(hop))
        game.cmd('fly', 'on')
    game.cmd('time', 'set', hour)
    game.cmd('time', 'freeze')
    end = time.monotonic() + args.look_wait
    while 'error' in game.cmd('look', yaw, pitch, ok=False):  # (no camera for a moment after a hop)
        if time.monotonic() > end:
            raise RuntimeError('look: no camera')
        time.sleep(1)
    look = game.poll(['look', 'status'], ('done', 'failed', 'timeout', 'idle'), 15)
    time.sleep(args.settle)
    game.cmd('look', yaw, pitch)  # (again: the camera can drift while the area streams in)
    look = game.poll(['look', 'status'], ('done', 'failed', 'timeout', 'idle'), 10)
    time.sleep(1)
    # 1. frame times, GPU time and CPU time per thread, with nothing else running
    gpu = []
    stop = threading.Event()
    sampler = threading.Thread(target=gpu_sampler, args=(stop, gpu), daemon=True)
    sampler.start()
    log_a = os.path.getsize(log_path)
    trace = not args.no_trace
    mem_first = game.cmd('memstats', ok=False) if trace else {}
    mem_first = mem_first if 'error' not in mem_first else {}
    if trace and not args.trace_from_boot:
        game.cmd('frametrace', 'start', ok=False)
    th_a = game.cmd('threads')
    polls, end = [], time.monotonic() + args.duration
    while time.monotonic() < end:  # (memstats once a second: a few tens of microseconds on the presenting thread)
        m = game.cmd('memstats', ok=False) if trace else {'error': 'off'}
        if 'error' not in m:
            polls.append(m)
        time.sleep(max(0.0, min(1.0, end - time.monotonic())))
    th_b = game.cmd('threads')
    trace_rel = 'build/frametrace_{}.csv'.format(os.getpid())
    dump = game.cmd('frametrace', 'dump', trace_rel, ok=False) if trace else {'error': 'off'}
    traced = 'error' not in dump
    mem_last = polls[-1] if polls else {}
    frames = game.cmd('frames', min(args.duration, 60))
    time.sleep(1.2)  # (the next fps line, which covers the end of the window)
    log_b = os.path.getsize(log_path)
    stop.set()
    # 2. the view, the GPU time per pass of one frame, then stack samples (both slow the game a little)
    pos = game.cmd('pos', ok=False)
    shot_rel = 'build/perf_routes_{}.png'.format(os.getpid())
    game.cmd('shot', shot_rel, ok=False)
    prof_at = os.path.getsize(log_path)
    game.cmd('gpuprof')
    time.sleep(1.5)
    shot_dst = os.path.join(run_dir, '{}.png'.format(tag))
    try:
        shutil.move(os.path.join(game_cwd(args), shot_rel), shot_dst)
    except OSError:
        shot_dst = ''
    gp_total, gp_top = gpuprof(log_path, prof_at, args.top)
    hitch = {'hitches': -1, 'hitch_causes': '', 'hitch_ms': -1}
    if traced:
        trace_dst = os.path.join(run_dir, '{}_frames.csv'.format(tag))
        shutil.move(os.path.join(game_cwd(args), trace_rel), trace_dst)
        hitch = hitch_analysis(trace_dst, os.path.join(run_dir, '{}_hitches.csv'.format(tag)))
    shares = {}
    if args.sample > 0 and os.name == 'nt':
        game.cmd('sample', 'start', args.sample, args.sample_us)
        time.sleep(args.sample)
        if game.poll(['sample', 'status'], ('done',), 120) == 'done':
            shares = sample_shares(args.tree, os.path.join(args.tree, 'build', 'samples.txt'))

    presents = th_b['presents'] - th_a['presents']
    cpu = {k: (th_b[k] - th_a[k]) / presents for k in th_b
           if k in th_a and k not in ('now_ms', 'presents') and th_a[k] >= 0} if presents else {}
    rows, hitches = vk_lines(log_path, log_a, log_b)
    rows = rows[1:] if len(rows) > 1 else rows  # (the first line's interval began before the window)
    nfr = rows[-1]['frame'] - rows[0]['frame'] if len(rows) > 1 else 0
    tot = lambda k: sum(r[k] for r in rows[1:])
    frame_ms = frames['frame_ms_avg']
    sh = lambda k: shares.get(k, -1)
    cpu_main = next((v for k, v in cpu.items() if k.startswith('guest.101_')), -1)
    cpu_render = cpu.get('host.render', -1)
    # Without stack samples (Linux): main and render busy = their CPU time (neither spins while it waits)
    game_ms = frame_ms * sh('main_busy') if shares else cpu_main
    render_ms = frame_ms * sh('render_busy') if shares else cpu_render
    res = {
        'route': name, 'fps_avg': frames['fps_avg'], 'low1_fps': 1000 / frames['frame_ms_p99'],
        'frame_ms_avg': frame_ms, 'p99_ms': frames['frame_ms_p99'], 'max_ms': frames['frame_ms_max'],
        'over33': frames['over_33_ms'], 'over50': frames['over_50_ms'],
        'game_ms': game_ms, 'render_ms': render_ms, 'busy_from': 'samples' if shares else 'cpu',
        'recorder_ms': cpu.get('host.recorder', -1),
        'gpu_ms': tot('gpu') / nfr if nfr else -1,
        'main_wait_render': sh('main_wait_render'), 'main_wait_gpu': sh('main_wait_gpu'),
        'main_wait_other': sh('main_wait_other'), 'render_wait_gpu': sh('render_wait_gpu'),
        'render_idle': sh('render_idle'),
        'exec_ms': tot('exec') / nfr if nfr else -1, 'gpu_wait_ms': tot('wait') / nfr if nfr else -1,
        'cpu_ms': sum(cpu.values()),
        'cpu_main_ms': cpu_main, 'cpu_render_ms': cpu_render,
        'spinning': sum(1 for k, v in cpu.items() if k.startswith('guest.') and frame_ms and v / frame_ms > 0.9),
        'compiles': int(sum(r['compile'] for r in rows)),
        'compile_ms': sum(r['tr_ms'] + r['pipe_ms'] for r in rows), 'hitches': hitches,
        'gpu_clock_mhz': statistics.mean(g[0] for g in gpu) if gpu else -1,
        'cpu_clock_mhz': statistics.mean(g[3] for g in gpu) if gpu and len(gpu[0]) > 3 else -1,
        'gpu_util_pct': statistics.mean(g[1] for g in gpu) if gpu else -1,
        'gpu_power_w': statistics.mean(g[2] for g in gpu) if gpu else -1,
        'gpuprof_ms': gp_total if gp_total is not None else -1, 'top_passes': gp_top,
        'look': look, 'place': pos.get('place', '') if isinstance(pos, dict) else '', 'shot': shot_dst,
    }
    res.update(mem_summary(polls, mem_first, mem_last))
    res['trace_cost_us'] = dump.get('cost_us_avg', -1) if traced else -1
    res['memstats_us'] = max((p.get('self_us', -1) for p in polls), default=-1)
    res.update(hitch)
    res['limiter'] = limiter(res)
    return res


def limiter(r):
    """The busiest of main thread, render thread and GPU, as a share of the frame time (under 80%: none)."""
    f = r['frame_ms_avg']
    if f <= 0 or r['game_ms'] < 0:
        return '?'
    parts = {'game': r['game_ms'], 'render': r['render_ms'], 'gpu': r['gpu_ms'], 'recorder': r.get('recorder_ms', -1)}
    name, ms = max(parts.items(), key=lambda kv: kv[1])
    share = ms / f
    return '{} {:.0f}%'.format(name, share * 100) if share >= 0.8 else 'none ({} {:.0f}%)'.format(name, share * 100)


FIELDS = ['config', 'size', 'run', 'route', 'fps_avg', 'low1_fps', 'frame_ms_avg', 'p99_ms', 'max_ms', 'over33', 'over50',
          'game_ms', 'render_ms', 'gpu_ms', 'limiter', 'main_wait_render', 'main_wait_gpu', 'main_wait_other',
          'render_wait_gpu', 'render_idle', 'exec_ms', 'gpu_wait_ms', 'cpu_ms', 'cpu_main_ms', 'cpu_render_ms',
          'spinning', 'compiles', 'compile_ms', 'hitches', 'gpu_clock_mhz', 'gpu_util_pct', 'gpu_power_w', 'gpuprof_ms',
          'top_passes', 'look', 'place', 'others', 'shot',
          # memory (memstats) and hitches (frametrace)
          'busy_from', 'recorder_ms', 'ram_peak_mb', 'ram_med_mb', 'ram_hwm_mb', 'sys_used_peak_mb', 'gpu_local_peak_mb',
          'gpu_local_med_mb', 'gpu_host_peak_mb', 'gpu_host_med_mb', 'heaps', 'tex_astc_native_mb', 'tex_astc_rgba8_mb',
          'tex_astc_src_mb', 'tex_bc_mb', 'tex_rt_mb', 'tex_other_mb', 'astc_ms', 'median_ms', 'hitches', 'hitch_ms',
          'hitch_causes', 'win_builds', 'win_build_wait_ms', 'win_upload_mb', 'win_file_mb', 'win_faults', 'trace_cost_us',
          'memstats_us', 'cpu_clock_mhz']




def stop_game(game, proc, log):
    if game:
        try:
            game.cmd('exit', ok=False, timeout=3)
        except (OSError, ValueError, RuntimeError):
            pass
        game.close()
    try:
        proc.wait(20)
    except subprocess.TimeoutExpired:
        if os.name == 'nt':
            proc.kill()  # (our own process, by its handle)
        else:
            try:
                os.killpg(proc.pid, signal.SIGKILL)  # (its own process group: the AppImage runtime and the game)
            except OSError:
                pass
        proc.wait(10)
    log.close()
    time.sleep(3)  # (a game that just ended can keep the next one from starting)


def run_route(args, w, f, size, i, route, attempt):
    tag = '{}_{}_r{}_{}'.format(args.config, size, i, route[0])
    run_dir = os.path.join(args.out, tag + ('' if attempt == 1 else '_attempt{}'.format(attempt)))
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir)
    while os.name != 'nt' and others(args.tree):  # (Linux: the player is playing; wait)
        print('{:34} waiting: savagewind running ({})'.format(tag, ', '.join(others(args.tree))), flush=True)
        time.sleep(30)
    busy = others(args.tree)
    proc, log = launch(args, run_dir, size, args.affinity, args.env + ['SAVAGEWIND_TEST_WARP=' + route[1]])
    game = None
    try:
        game = Game(args.port)
        wait_world(game, proc, args.boot)
        if args.trace_from_boot and not args.no_trace:  # (first-boot hitches: from the world's first frames on)
            game.cmd('frametrace', 'start', ok=False)
        time.sleep(args.after_boot)
        res = measure(game, args, game_log(args, run_dir), route, run_dir, tag)
        res.update(config=args.config, size=size, run=i, others=len(set(busy) | set(others(args.tree))))
        if attempt > 1:
            res['look'] += ' (attempt {})'.format(attempt)
        w.writerow({k: round(v, 2) if isinstance(v, float) else v for k, v in res.items()})
        f.flush()
        print('{tag:34} {fps_avg:6.1f} fps  1% {low1_fps:6.1f}  p99 {p99_ms:5.1f}  game {game_ms:5.2f}  '
              'render {render_ms:5.2f}  gpu {gpu_ms:5.2f} ms  {limiter}  cpu {cpu_ms:5.1f} ms '
              '(spin {spinning})  compiles {compiles}  clock {gpu_clock_mhz:.0f}  others {others}  ram {ram_peak_mb:.0f} MB  '
              'gpu {gpu_local_peak_mb:.0f}+{gpu_host_peak_mb:.0f} MB  hitches {hitches} ({hitch_causes})'.format(tag=tag, **res),
              flush=True)
        return True
    except (OSError, ValueError, RuntimeError) as exc:
        print('{:34} failed: {}'.format(tag, exc), flush=True)
        return False
    finally:
        stop_game(game, proc, log)
        if args.data_root:  # (the game's own log, kept with the run)
            try:
                shutil.copyfile(game_log(args, run_dir), os.path.join(run_dir, 'game.log'))
            except OSError:
                pass


def run(args):
    os.makedirs(args.out, exist_ok=True)
    out_csv = os.path.join(args.out, 'results.csv')
    new = not os.path.exists(out_csv)
    routes = [r for r in ROUTES if not args.routes or r[0] in args.routes]
    with open(out_csv, 'a', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, FIELDS, extrasaction='ignore')
        if new:
            w.writeheader()
        for i in range(args.first_run, args.first_run + args.runs):
            for size in args.size:
                for route in routes:
                    for attempt in (1, 2):  # (one retry: a run whose game crashed or lost its camera)
                        if run_route(args, w, f, size, i, route, attempt):
                            break
    return 0


def boot(args):
    """Start the game as a run would and leave it running, to check a spot by hand (swctl --port, shot)."""
    run_dir = os.path.join(args.out, 'boot')
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir)
    proc, log = launch(args, run_dir, args.size[0], args.affinity, args.env + ['SAVAGEWIND_TEST_WARP=' + args.place])
    game = Game(args.port)
    wait_world(game, proc, args.boot)
    game.close()
    print('pid {} port {} log {}'.format(proc.pid, args.port, os.path.join(run_dir, 'log.txt')))
    return 0


def rehitch(args):
    """The hitch columns of results.csv worked out again from each run's <tag>_frames.csv."""
    import glob
    for d in args.dirs:
        path = os.path.join(d, 'results.csv')
        with open(path, encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
            fields = list(rows[0].keys()) if rows else FIELDS
        for r in rows:
            tag = '{}_{}_r{}_{}'.format(r['config'], r['size'], r['run'], r['route'])
            traces = sorted(glob.glob(os.path.join(d, tag + '*', tag + '_frames.csv')))
            if traces:
                r.update({k: round(v, 2) if isinstance(v, float) else v for k, v in
                          hitch_analysis(traces[-1], traces[-1].replace('_frames.csv', '_hitches.csv')).items()})
        with open(path, 'w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fields, extrasaction='ignore')
            w.writeheader()
            w.writerows(rows)
    return 0


def table(args):
    rows = []
    for d in args.dirs:
        with open(os.path.join(d, 'results.csv'), encoding='utf-8') as f:
            rows += list(csv.DictReader(f))
    groups = {}
    for r in rows:
        groups.setdefault((r['config'], r['size'], r['route']), []).append(r)
    print('| config | size | route | runs | fps avg (min-max) | 1% low | p99 ms | max ms | main busy ms | render busy ms '
          '| GPU ms | main waits render/GPU/other | CPU ms/frame | compiles | limits | top GPU passes (last run) |')
    print('|' + '---|' * 16)
    for (cfg, size, route), g in groups.items():
        num = lambda k: [float(r[k]) for r in g]
        med = lambda k: statistics.median(num(k))
        fps = num('fps_avg')
        print('| {} | {} | {} | {} | {:.1f} ({:.1f}-{:.1f}) | {:.1f} | {:.1f} | {:.1f} | {:.2f} | {:.2f} | {:.2f} '
              '| {} | {:.1f} | {} | {} | {} |'.format(
                  cfg, size, route, len(g), statistics.median(fps), min(fps), max(fps), med('low1_fps'), med('p99_ms'),
                  med('max_ms'), med('game_ms'), med('render_ms'), med('gpu_ms'),
                  '{:.0f}/{:.0f}/{:.0f}%'.format(100 * med('main_wait_render'), 100 * med('main_wait_gpu'),
                                                100 * med('main_wait_other')) if med('main_wait_render') >= 0
                  else 'n/a (CPU time)', med('cpu_ms'), int(sum(num('compiles'))),
                  ', '.join(sorted({r['limiter'] for r in g})), g[-1]['top_passes'].replace('|', '/')))
    mem = [(k, g) for k, g in groups.items() if g[0].get('ram_peak_mb') not in (None, '', '-1', '-1.0')]
    if mem:  # (rows from before the memory columns have none)
        print()
        print('| config | size | route | RAM peak / median MB | GPU local peak MB | GPU host peak MB | system used peak MB '
              '| tex ASTC native MB | tex ASTC->RGBA8 MB (ASTC size) | tex BC MB | tex RT MB | tex other MB '
              '| ASTC decode ms / window |')
        print('|' + '---|' * 13)
        for (cfg, size, route), g in mem:
            med = lambda k: statistics.median(float(r[k]) for r in g)
            print('| {} | {} | {} | {:.0f} / {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} | {:.0f} ({:.0f}) | {:.0f} | {:.0f} '
                  '| {:.0f} | {:.1f} |'.format(cfg, size, route, med('ram_peak_mb'), med('ram_med_mb'),
                                              med('gpu_local_peak_mb'), med('gpu_host_peak_mb'), med('sys_used_peak_mb'),
                                              med('tex_astc_native_mb'), med('tex_astc_rgba8_mb'), med('tex_astc_src_mb'),
                                              med('tex_bc_mb'), med('tex_rt_mb'), med('tex_other_mb'), med('astc_ms')))
        print()
        print('| config | size | route | runs | median frame ms | hitches (> {}x median) / run | hitch ms / run '
              '| causes (all runs) | builds / window | build waits ms | uploads MB | file reads MB |'.format(HITCH_X))
        print('|' + '---|' * 12)
        for (cfg, size, route), g in mem:
            num = lambda k: [float(r[k]) for r in g if r.get(k) not in (None, '')]
            causes = collections.Counter()
            for r in g:
                for c in (r.get('hitch_causes') or '').split():
                    k, v = c.rsplit(':', 1)
                    causes[k] += int(v)
            avg = lambda k: statistics.mean(num(k)) if num(k) else -1
            print('| {} | {} | {} | {} | {:.2f} | {:.1f} | {:.0f} | {} | {:.0f} | {:.1f} | {:.0f} | {:.1f} |'.format(
                cfg, size, route, len(g), avg('median_ms'), avg('hitches'), avg('hitch_ms'),
                ', '.join('{} {}'.format(k, v) for k, v in causes.most_common()) or 'none', avg('win_builds'),
                avg('win_build_wait_ms'), avg('win_upload_mb'), avg('win_file_mb')))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='action', required=True)
    rp = sub.add_parser('run')
    bp = sub.add_parser('boot')
    rp.add_argument('--tree', default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    rp.add_argument('--save', required=True, help='save folder to copy for each boot (holds save_0)')
    rp.add_argument('--ini', help='settings to start from (default: <tree>/savagewind.ini)')
    rp.add_argument('--out', required=True)
    rp.add_argument('--config', default='base', help='label for this configuration (CSV column)')
    rp.add_argument('--size', nargs='+', default=['2560x1440'])
    rp.add_argument('--runs', type=int, default=1)
    rp.add_argument('--first-run', type=int, default=1, help='number of the first run (to add runs to a results.csv)')
    rp.add_argument('--routes', nargs='*', help='only these routes')
    rp.add_argument('--duration', type=float, default=20)
    rp.add_argument('--settle', type=float, default=12)
    rp.add_argument('--boot', type=float, default=240, help='seconds to wait for a world')
    rp.add_argument('--after-boot', type=float, default=5)
    rp.add_argument('--port', type=int, default=7790)
    rp.add_argument('--affinity', type=lambda s: int(s, 0), default=0, help='CPU mask, e.g. 0xff (logical processors 0-7)')
    rp.add_argument('--env', nargs='*', default=[], help='extra VAR=value for the game')
    rp.add_argument('--top', type=int, default=3)
    rp.add_argument('--sample', type=float, default=5, help='seconds of stack samples for the thread shares (0: none)')
    rp.add_argument('--sample-us', type=int, default=2000, help='stack sample interval (us)')
    rp.add_argument('--exe', help='game executable (default: <tree>/build/savagewind[.exe]; Linux: an AppImage)')
    rp.add_argument('--data-root', help='SAVAGEWIND_DATA_ROOT for the game (a set-up data root of its own)')
    rp.add_argument('--cache-reset', help='before each boot, the data root\'s cache/ replaced by a copy of <dir>/cache '
                    'and the Mesa shader cache started from <dir>/mesa (or empty): a first boot after setup')
    rp.add_argument('--look-wait', type=float, default=30, help='seconds to wait for a camera after a boot or hop')
    rp.add_argument('--trace-from-boot', action='store_true', help='frame trace from the first world frames to the end '
                    'of the window (the hitches of a first boot), not just the window')
    rp.add_argument('--no-trace', action='store_true', help='no memstats / frametrace (to measure their own cost)')
    rp.add_argument('--visible', action='store_true', help='a visible window (default: SAVAGEWIND_HIDDEN=1)')
    rp.add_argument('--fullscreen', action='store_true', help='fullscreen (default: a window of --size)')
    bp.add_argument('--place', default='plateau', help='SAVAGEWIND_TEST_WARP place')
    for a in rp._actions[1:]:
        if a.dest not in ('runs', 'routes', 'duration', 'settle', 'after_boot', 'top', 'config', 'sample', 'sample_us', 'first_run'):
            bp._add_action(a)
    tp = sub.add_parser('table')
    tp.add_argument('dirs', nargs='+')
    hp = sub.add_parser('rehitch', help='analyse the kept frame traces again (after a change of the hitch rules)')
    hp.add_argument('dirs', nargs='+')
    args = ap.parse_args(argv)
    if os.name == 'nt':  # (nm for the stack samples' symbols; the game's runtime DLLs)
        os.environ['PATH'] = r'C:\msys64\ucrt64\bin;' + os.environ.get('PATH', '')
    if args.action in ('run', 'boot'):
        args.ini = args.ini or os.path.join(args.tree, 'savagewind.ini')
        return run(args) if args.action == 'run' else boot(args)
    return table(args) if args.action == 'table' else rehitch(args)


if __name__ == '__main__':
    sys.exit(main())
