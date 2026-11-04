#!/usr/bin/env python3
"""Steam Deck performance diagnostic: runs the game a few times, measures frame times and where the CPU and GPU time
goes in the same place of the world, and packs it into one archive (docs/linux.md, "Steam Deck diagnostic").

Usage: python3 tools/deck_diag.py [--game PATH] [--quick] [--seconds N]

Each run starts the game with the control endpoint (docs/CONTROL.md). In the first one, load a save, walk to an open
part of the world (the Great Plateau is good), and let go of the controls: once Link has stood still for a few seconds
the script keeps that place and puts him back there in the next runs by itself. Don't touch anything until the game
closes. Needs only Python 3.8 or later. The game's own settings file is not touched (a copy is used).
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tarfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swctl  # noqa: E402

PORT = 7777

# One game start each: what needs a restart (environment). SAVAGEWIND_STEAM_DECK=0 turns every Deck-only change off.
RUNS = [
    ('deck', {}),
    ('pc', {'SAVAGEWIND_STEAM_DECK': '0'}),
    ('deck_submit300', {'SAVAGEWIND_VK_SUBMIT_DRAWS': '300'}),
    ('deck_compile_default', {'SAVAGEWIND_DECK_COMPILE_THREADS': '0'}),
]
# Within a start: settings changed live (control "set"). No frame cap and no VSync, so the numbers are what the Deck
# can draw, not what the cap allows.
BASE = [('fps', '0'), ('vsync', 'off'), ('frame_gen', 'off'), ('shadow_res', 'console'), ('antialias', 'off')]
VARIANTS = [
    ('native', [('upscaler', 'off')]),
    ('fsr1_ultra_quality', [('upscaler', 'fsr1'), ('upscale_quality', 'ultra_quality')]),
    ('fsr1_performance', [('upscaler', 'fsr1'), ('upscale_quality', 'performance')]),
]


def log(msg):
    print(time.strftime('[%H:%M:%S] ') + msg, flush=True)


class Game:
    # The start screen runs the game by re-executing itself: a connection made to it closes then. So a command that
    # meets a closed connection connects again and is sent once more (every command used here can be repeated).
    def __init__(self, wait):
        self.conn = swctl.connect('127.0.0.1', PORT, 5, wait)

    def cmd(self, *words, timeout=10):
        for attempt in range(2):
            try:
                line = swctl.exchange(self.conn, swctl.request_line(list(words)), timeout)
                return swctl.parse_reply(line, words[0])
            except (OSError, ValueError):
                if attempt or words[0] == 'exit':
                    raise
                try:
                    self.conn.close()
                except OSError:
                    pass
                self.conn = swctl.connect('127.0.0.1', PORT, 5, 20 * 60)


def data_root():
    e = os.environ.get('SAVAGEWIND_DATA_ROOT')
    if e:
        return e
    xdg = os.environ.get('XDG_DATA_HOME', '')
    return os.path.join(xdg if xdg.startswith('/') else os.path.expanduser('~/.local/share'), 'savagewind')


def read(path, default=''):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            return f.read().strip()
    except OSError:
        return default


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError) as exc:
        return 'failed: ' + str(exc)


def system_info(out):
    parts = {
        'os': read('/etc/os-release'),
        'kernel': sh('uname -a'),
        'board': read('/sys/devices/virtual/dmi/id/board_vendor') + ' ' + read('/sys/devices/virtual/dmi/id/product_name'),
        'cpu': sh('lscpu'),
        'memory': sh('free -m'),
        'governor': sh('cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | sort | uniq -c'),
        'gpu_power_level': sh('cat /sys/class/drm/card*/device/power_dpm_force_performance_level'),
        'gpu_clocks': sh('cat /sys/class/drm/card*/device/pp_dpm_sclk'),
        'vulkan': sh('vulkaninfo --summary 2>/dev/null | head -60'),
        'env': '\n'.join(f'{k}={v}' for k, v in sorted(os.environ.items())
                         if k.startswith(('SAVAGEWIND', 'Steam', 'STEAM', 'RADV', 'MESA', 'DXVK', 'VKD3D',
                                          'XDG_SESSION', 'XDG_CURRENT', 'SDL_', 'WAYLAND', 'DISPLAY', 'GAMESCOPE'))),
    }
    with open(os.path.join(out, 'system.txt'), 'w', encoding='utf-8') as f:
        for k, v in parts.items():
            f.write(f'===== {k}\n{v}\n\n')
    return parts


# ---- sampling: the whole machine and each of the game's threads, once a second
def cpu_times():
    rows = {}
    for line in read('/proc/stat').splitlines():
        if line.startswith('cpu'):
            p = line.split()
            v = list(map(int, p[1:]))
            idle = v[3] + (v[4] if len(v) > 4 else 0)
            rows[p[0]] = (sum(v), idle)
    return rows


def thread_times(pid):
    rows = {}
    for d in glob.glob(f'/proc/{pid}/task/*'):
        s = read(d + '/stat')
        if not s:
            continue
        name = s[s.find('(') + 1:s.rfind(')')]
        f = s[s.rfind(')') + 2:].split()
        rows[os.path.basename(d)] = (name, int(f[11]) + int(f[12]))  # utime + stime (ticks)
    return rows


def first(pattern):
    m = sorted(glob.glob(pattern))
    return m[0] if m else None


class Sampler(threading.Thread):
    def __init__(self, pid):
        super().__init__(daemon=True)
        self.pid, self.rows, self.stop = pid, [], threading.Event()
        self.gpu_busy = first('/sys/class/drm/card*/device/gpu_busy_percent')
        self.sclk = first('/sys/class/drm/card*/device/pp_dpm_sclk')
        hw = first('/sys/class/drm/card*/device/hwmon/hwmon*')
        self.power = hw and (first(hw + '/power1_average') or first(hw + '/power1_input'))
        self.temp = hw and first(hw + '/temp1_input')
        self.tick = os.sysconf('SC_CLK_TCK')

    def run(self):
        c0, t0, w0 = cpu_times(), thread_times(self.pid), time.monotonic()
        while not self.stop.wait(1.0):
            c1, t1, w1 = cpu_times(), thread_times(self.pid), time.monotonic()
            cpus = {k: round(100 * (1 - (c1[k][1] - c0[k][1]) / max(1, c1[k][0] - c0[k][0])), 1)
                    for k in c1 if k in c0}
            threads = sorted(((round(100 * (t1[k][1] - t0[k][1]) / self.tick / (w1 - w0), 1), t1[k][0], k)
                              for k in t1 if k in t0), reverse=True)[:8]
            sclk = next((l.split()[1] for l in read(self.sclk).splitlines() if l.endswith('*')), '') if self.sclk else ''
            self.rows.append({
                't': round(w1, 2), 'cpu_total': cpus.get('cpu'),
                'cpu_each': [cpus[k] for k in sorted(cpus) if k != 'cpu'],
                'threads': [{'pct': p, 'name': n, 'tid': tid} for p, n, tid in threads],
                'gpu_busy': int(read(self.gpu_busy, '-1')) if self.gpu_busy else None,
                'gpu_clock': sclk,
                'power_w': round(int(read(self.power, '0')) / 1e6, 2) if self.power else None,
                'temp_c': round(int(read(self.temp, '0')) / 1e3, 1) if self.temp else None,
            })
            c0, t0, w0 = c1, t1, w1

    def window(self, since):
        return [r for r in self.rows if r['t'] >= since]


def avg(values):
    values = [v for v in values if isinstance(v, (int, float)) and v >= 0]
    return round(sum(values) / len(values), 1) if values else None


def summarize(rows):
    top = {}
    for r in rows:
        for t in r['threads']:
            top.setdefault((t['name'], t['tid']), []).append(t['pct'])
    busiest = sorted(((round(sum(v) / len(rows), 1), n, tid) for (n, tid), v in top.items()), reverse=True)[:5]
    return {
        'cpu_total_pct': avg([r['cpu_total'] for r in rows]),
        'busiest_cpu_pct': avg([max(r['cpu_each']) for r in rows if r['cpu_each']]),
        'threads_pct': [f'{n}/{tid}: {p}' for p, n, tid in busiest],
        'gpu_busy_pct': avg([r['gpu_busy'] for r in rows]),
        'gpu_clock': sorted({r['gpu_clock'] for r in rows if r['gpu_clock']}),
        'power_w': avg([r['power_w'] for r in rows]),
        'temp_c': avg([r['temp_c'] for r in rows]),
    }


# ---- one game start
def wait_still(game, deadline, still_s=6):
    last, since = None, None
    while time.monotonic() < deadline:
        r = game.cmd('pos')
        if r['ok']:
            p = (round(r['data']['x'], 1), round(r['data']['y'], 1), round(r['data']['z'], 1))
            if p != last:
                last, since = p, time.monotonic()
            elif time.monotonic() - since >= still_s:
                return r['data']['place']
        time.sleep(1)
    return None


def one_start(name, env_extra, args, out, place, settings_file):
    env = dict(os.environ, SAVAGEWIND_CONTROL=str(PORT), SAVAGEWIND_FRAME_LOG='2', SAVAGEWIND_SETTINGS=settings_file,
               **env_extra)
    stderr_path = os.path.join(out, name + '.stderr.log')
    started = time.time()
    log(f'run "{name}" ({" ".join(f"{k}={v}" for k, v in env_extra.items()) or "Deck defaults"}): starting the game')
    with open(stderr_path, 'w') as errf:
        proc = subprocess.Popen([os.path.abspath(args.game)], env=env, stdout=errf, stderr=subprocess.STDOUT)
    result = {'name': name, 'env': env_extra, 'variants': {}}
    sampler = Sampler(proc.pid)
    sampler.start()
    try:
        game = Game(20 * 60)
        deadline = time.monotonic() + 20 * 60
        if place is None:
            log('LOAD YOUR SAVE, walk to an open place and LET GO of the controls (waiting for Link to stand still)')
            place = wait_still(game, deadline)
            if not place:
                raise RuntimeError('no save loaded within 20 minutes')
            log('place kept: ' + place)
        else:
            log('LOAD YOUR SAVE (any one: Link is moved to the first run\'s place), then let go of the controls')
            while time.monotonic() < deadline and not game.cmd('pos')['ok']:
                time.sleep(2)
            time.sleep(5)
            r = game.cmd('goto', place)
            log('goto: ' + ('ok' if r['ok'] else r.get('error', '?')))
            time.sleep(8)
        game.cmd('time', 'set', '12')
        game.cmd('time', 'freeze')
        result['place'] = place
        result['report'] = game.cmd('report').get('data')
        for k, v in BASE:
            game.cmd('set', k, v)
        variants = VARIANTS if not args.quick and name in ('deck', 'pc') else VARIANTS[1:2]
        for vname, sets in variants:
            applied = [game.cmd('set', k, v) for k, v in sets]
            log(f'  {vname}: settling')
            time.sleep(args.warmup)
            t0 = time.monotonic()
            time.sleep(args.seconds)
            frames = game.cmd('frames', str(int(args.seconds)))
            res = {'frames': frames.get('data') or frames.get('error'),
                   'set': [a.get('data') or a.get('error') for a in applied],
                   'machine': summarize(sampler.window(t0))}
            result['variants'][vname] = res
            f = res['frames'] if isinstance(res['frames'], dict) else {}
            m = res['machine']
            log(f'  {vname}: {f.get("fps_avg", 0):.1f} fps, p95 {f.get("frame_ms_p95", 0):.1f} ms | GPU '
                f'{m["gpu_busy_pct"]}% | CPU {m["cpu_total_pct"]}% (busiest thread {m["threads_pct"][:1]})')
        game.cmd('time', 'unfreeze')
        game.cmd('exit')
    except Exception as exc:  # (a run that fails still keeps its logs)
        result['error'] = repr(exc)
        log(f'run "{name}" failed: {exc!r}')
    finally:
        sampler.stop.set()
        try:
            proc.wait(30)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
    with open(os.path.join(out, name + '.samples.json'), 'w') as f:
        json.dump(sampler.rows, f)
    game_log = os.path.join(data_root(), 'savagewind.log')  # (started from a folder with extracted/: stderr.log has it)
    if os.path.exists(game_log) and os.path.getmtime(game_log) >= started:
        shutil.copy(game_log, os.path.join(out, name + '.game.log'))
    return result, place


def default_game():
    for p in ['build/savagewind', *sorted(glob.glob(os.path.expanduser('~/*/Savage_Wind*.AppImage'))),
              *sorted(glob.glob(os.path.expanduser('~/Savage_Wind*.AppImage')))]:
        if os.path.isfile(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=default_game(), help='build/savagewind or the AppImage')
    ap.add_argument('--quick', action='store_true', help='only the Deck and PC runs, one setting each')
    ap.add_argument('--seconds', type=float, default=20, help='measured seconds per setting (default 20)')
    ap.add_argument('--warmup', type=float, default=6, help='seconds after a change before measuring (default 6)')
    args = ap.parse_args()
    if not args.game or not os.path.isfile(args.game):
        ap.error('game not found: pass --game build/savagewind or --game ~/Downloads/Savage_Wind-....AppImage')
    stamp = time.strftime('%Y%m%d-%H%M%S')
    out = os.path.abspath(f'savagewind-deck-diag-{stamp}')
    os.makedirs(out)
    log('output: ' + out)
    info = system_info(out)
    settings_file = os.path.join(out, 'diag.ini')
    ini = os.path.join(data_root(), 'savagewind.ini')
    if os.path.exists('savagewind.ini') and os.path.isdir('extracted'):
        ini = 'savagewind.ini'
    if os.path.exists(ini):
        shutil.copy(ini, settings_file)
        shutil.copy(ini, os.path.join(out, 'player_settings.ini'))
    # Options read once at start, the same in every run (a later line of the file wins): the Deck and PC runs differ
    # only in the code's Deck changes, not in a first launch's defaults.
    with open(settings_file, 'a', encoding='utf-8') as f:
        f.write('\nsettings_version = 3\nasync_shaders = 1\nprecompile_shaders = 1\nsharpening = 0\n'
                'frame_rate = off\nvsync = off\nfullscreen = 1\nmenu_tip_seen = 1\n')
    runs = RUNS[:2] if args.quick else RUNS
    results, place = [], None
    for name, env_extra in runs:
        r, place = one_start(name, env_extra, args, out, place, settings_file)
        results.append(r)
        if place is None:
            break
    with open(os.path.join(out, 'results.json'), 'w') as f:
        json.dump(results, f, indent=2)
    lines = [f'Savage Wind Steam Deck diagnostic {stamp}', 'board: ' + info['board'],
             'session: ' + os.environ.get('XDG_CURRENT_DESKTOP', '?') + (' (Gaming Mode)' if os.environ.get('GAMESCOPE_WAYLAND_DISPLAY') else ''),
             'place: ' + str(place), '']
    for r in results:
        lines.append(f'== {r["name"]} {r["env"] or ""} {"ERROR " + r["error"] if r.get("error") else ""}')
        for vname, v in r['variants'].items():
            f, m = v['frames'] if isinstance(v['frames'], dict) else {}, v['machine']
            lines.append(f'  {vname:20} {f.get("fps_avg", 0):6.1f} fps  p50 {f.get("frame_ms_p50", 0):5.1f}  '
                         f'p95 {f.get("frame_ms_p95", 0):5.1f}  p99 {f.get("frame_ms_p99", 0):5.1f} ms  '
                         f'GPU {m["gpu_busy_pct"]}% {",".join(m["gpu_clock"])}  CPU {m["cpu_total_pct"]}% '
                         f'(busiest core {m["busiest_cpu_pct"]}%)  {m["power_w"]} W  {m["temp_c"]} C')
            lines.append('      threads: ' + ' | '.join(m['threads_pct']))
    summary = '\n'.join(lines) + '\n'
    with open(os.path.join(out, 'summary.txt'), 'w') as f:
        f.write(summary)
    archive = out + '.tar.gz'
    with tarfile.open(archive, 'w:gz') as t:
        t.add(out, arcname=os.path.basename(out))
    print('\n' + summary)
    log('done: send ' + archive + ' (or paste summary.txt)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
