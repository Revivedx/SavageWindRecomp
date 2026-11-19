#!/usr/bin/env python3
"""Extensive training run for the runtime's PGO profile (tools/pgo/README.md).

perf_routes.py measures fixed views: 20 seconds in clear weather at hour 10, about 40 seconds a boot. That is a good
benchmark and a thin training set. This script boots the instrumented runtime once per place and plays a longer list of
scenes in the same boot, so one profile covers what the player meets:

  day, a camera pan, a fly-around that streams in new ground, the pause menu and the map, dusk, night, rain, a
  thunderstorm, snow, dawn and the day again, with the player's own settings (SMAA, FSR 1, ultra shadows) at the main
  monitor's size, and with TAA at 1080p for a second set of upscaler and anti-aliasing code.

Each boot's `exit` command makes the instrumented runtime write its profile (runtime/control.c `pgo_dump`), so no scene
is lost to the 3000-frame dump interval. Run it with LLVM_PROFILE_FILE=<dir>/rt-%p.profraw, then merge the non-empty
files (see README.md). Standard library only; uses perf_routes.py for the boot, the control socket and the cleanup.

  train.py --exe build-gen/savagewind.exe --tree <worktree> --data-root <own data root> --save <save folder with save_0>
           --ini <player's savagewind.ini> --out <dir> --profiles <dir> [--quick] [--configs play taa1080] [--routes ...]
"""
import argparse
import glob
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import perf_routes as pr  # noqa: E402

# label, weather, hour, seconds, action. The scenes run in this order after the world is up.
SCENES = [
    ('day',       'clear',        10.0, 20, 'still'),
    ('day-pan',   'clear',        10.0, 35, 'pan'),
    ('day-fly',   'clear',        10.0, 40, 'fly'),
    ('menu',      'clear',        10.0, 20, 'menu'),
    ('dusk',      'cloudy',       18.5, 25, 'pan'),
    ('night',     'clear',        23.5, 30, 'pan'),
    ('night-fly', 'clear',        23.5, 25, 'fly'),
    ('rain',      'heavy_rain',   13.0, 30, 'pan'),
    ('storm',     'thunderstorm', 15.0, 30, 'fly'),
    ('snow',      'heavy_snow',   11.0, 25, 'pan'),
    ('dawn',      'clear',         5.5, 20, 'pan'),
    ('day-end',   'clear',        10.0, 15, 'still'),
]
SHORT = ('day', 'day-pan', 'day-fly', 'menu', 'night', 'rain')  # the second configuration: fewer scenes
QUICK = {'day': 8, 'day-fly': 10, 'menu': 8, 'rain': 8}          # --quick: a smoke test of the whole chain

# name, window size, settings changed in the player's ini, places
CONFIGS = {
    'play':    ('2560x1440', {}, None),  # the player's own settings
    'taa1080': ('1920x1080', {'antialias': 'taa', 'upscaler': 'off', 'render_scale': '100', 'shadow_resolution': 'high'},
                ('plateau', 'kakariko', 'field', 'hateno')),
}


def command(game, *words):
    """A control command whose failure is not the end of the run: a scene without its effect still runs the renderer."""
    try:
        return game.cmd(*words, ok=False, timeout=10)
    except (OSError, ValueError, RuntimeError):
        return {'error': 'no answer'}


def alive(proc):
    if proc.poll() is not None:
        raise RuntimeError('the game exited (code {:#x})'.format(proc.returncode & 0xffffffff))


def pause(proc, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        alive(proc)
        time.sleep(min(1.0, max(0.0, end - time.monotonic())))


def pan(game, proc, seconds, yaw, pitch):
    end, step = time.monotonic() + seconds, 0
    while time.monotonic() < end:
        alive(proc)
        yaw = (yaw + 25.0) % 360.0
        command(game, 'look', round(yaw, 1), pitch + (10 if step % 8 < 4 else -6))
        time.sleep(0.6)
        step += 1


def fly(game, proc, seconds, yaw, pitch):
    """Fly mode with the camera as the heading: straight for a few steps, then a turn, up and down now and then."""
    command(game, 'fly', 'on')
    command(game, 'fly', 'mode', 'look')
    end, step = time.monotonic() + seconds, 0
    while time.monotonic() < end:
        alive(proc)
        if step % 4 == 3:
            yaw = (yaw + 70.0) % 360.0
        command(game, 'look', round(yaw, 1), pitch - 4)
        command(game, 'stick', 'l', 0, 1, 40)
        if step % 5 == 4:
            command(game, 'press', 'x' if step % 10 == 4 else 'b', 40)
        time.sleep(1.2)
        step += 1


def menu(game, proc, seconds):
    """The pause menu (+), a few cursor moves, then the map (-) with a pan and a zoom, then back to the world."""
    command(game, 'press', 'plus', 3)
    pause(proc, 3)
    for button in ('right', 'right', 'left', 'down', 'up', 'left'):
        command(game, 'press', button, 3)
        pause(proc, 1)
    command(game, 'press', 'b', 3)
    pause(proc, 2)
    command(game, 'press', 'minus', 3)
    pause(proc, 3)
    for x, y in ((1, 0), (0, 1), (-1, 0), (0, -1)):
        command(game, 'stick', 'l', x, y, 20)
        pause(proc, 1)
    command(game, 'press', 'zr', 20)
    pause(proc, 1.5)
    command(game, 'press', 'zl', 20)
    pause(proc, 1.5)
    command(game, 'press', 'b', 3)
    pause(proc, 2)
    command(game, 'press', 'b', 3)
    pause(proc, max(0.0, seconds - 31))


def play_scene(game, proc, scene, route, quick):
    label, weather, hour, seconds, action = scene
    seconds = QUICK[label] if quick else seconds
    _, _, yaw, pitch, _, _ = route
    command(game, 'weather', weather)
    command(game, 'time', 'set', hour)
    command(game, 'time', 'freeze')
    command(game, 'look', yaw, pitch)
    pause(proc, 3)
    {'still': lambda: pause(proc, seconds),
     'pan': lambda: pan(game, proc, seconds, yaw, pitch),
     'fly': lambda: fly(game, proc, seconds, yaw, pitch),
     'menu': lambda: menu(game, proc, seconds)}[action]()


def frames_so_far(game):
    state = command(game, 'state')
    return state.get('frame', 0) if isinstance(state, dict) else 0


def ini_for(base, changes, size, path):
    out = []
    with open(base, encoding='utf-8') as f:
        for line in f:
            key = line.split('=', 1)[0].strip()
            out.append('{} = {}\n'.format(key, changes[key]) if key in changes else line)
    with open(path, 'w', encoding='utf-8') as f:
        f.writelines(out)


def one_boot(args, config, route, scenes, attempt):
    size, changes, _ = CONFIGS[config]
    tag = '{}_{}{}'.format(config, route[0], '' if attempt == 1 else '_attempt{}'.format(attempt))
    run_dir = os.path.join(args.out, tag)
    shutil.rmtree(run_dir, ignore_errors=True)
    os.makedirs(run_dir)
    ini = os.path.join(run_dir, 'base.ini')
    ini_for(args.base_ini, changes, size, ini)
    args.ini = ini
    before = set(glob.glob(os.path.join(args.profiles, '*.profraw')))
    started = time.monotonic()
    proc, log = pr.launch(args, run_dir, size, 0, args.env + ['SAVAGEWIND_TEST_WARP=' + route[1]])
    game, frames, ok = None, 0, False
    try:
        game = pr.Game(args.port)
        pr.wait_world(game, proc, args.boot)
        pause(proc, 5)
        if route[5]:  # a hop (rito, castle): a short teleport from the warp place with fly mode on
            command(game, 'teleport', *route[5])
            try:
                game.poll(['teleport', 'status'], ('done', 'failed', 'idle'), 30)
            except (OSError, ValueError, RuntimeError):
                pass
            command(game, 'fly', 'on')
        for scene in scenes:
            play_scene(game, proc, scene, route, args.quick)
        frames = frames_so_far(game)
        ok = True
    except (OSError, ValueError, RuntimeError) as exc:
        print('{:28} failed: {}'.format(tag, exc), flush=True)
    finally:
        code = proc.poll()
        pr.stop_game(game, proc, log)
    if not ok:  # (what the game said last, and how it ended: a crash is not told by the control socket)
        try:
            with open(os.path.join(run_dir, 'log.txt'), 'rb') as f:
                f.seek(max(0, os.path.getsize(f.name) - 1500))
                print('    game exit code {}; log ends: {}'.format(
                    'still running' if code is None else '{:#x}'.format(code & 0xffffffff),
                    f.read().decode('utf-8', 'replace').strip().splitlines()[-4:]), flush=True)
        except OSError:
            pass
    new = [p for p in glob.glob(os.path.join(args.profiles, '*.profraw')) if p not in before]
    sizes = sorted((os.path.getsize(p), os.path.basename(p)) for p in new)
    print('{:28} {}  {:5.1f} min  {} presented frames  profraw: {}'.format(
        tag, 'ok    ' if ok else 'FAILED', (time.monotonic() - started) / 60, frames,
        ', '.join('{} {} KB'.format(n, s // 1024) for s, n in sizes) or 'none'), flush=True)
    return ok and any(s > 0 for s, _ in sizes)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True, help='the instrumented runtime (make PGO=gen B=build-gen)')
    ap.add_argument('--tree', required=True, help='the worktree (the game\'s working folder)')
    ap.add_argument('--data-root', required=True, help='SAVAGEWIND_DATA_ROOT: its own set-up data root')
    ap.add_argument('--save', required=True, help='a scratch save folder holding save_0 (copied for each boot)')
    ap.add_argument('--ini', required=True, help='the player\'s savagewind.ini to start from')
    ap.add_argument('--out', required=True)
    ap.add_argument('--profiles', required=True, help='where LLVM_PROFILE_FILE writes (checked for each boot\'s file)')
    ap.add_argument('--configs', nargs='+', default=['play', 'taa1080'], choices=sorted(CONFIGS))
    ap.add_argument('--routes', nargs='*', help='only these places (default: every route of perf_routes.py)')
    ap.add_argument('--quick', action='store_true', help='a short run of a few scenes (smoke test)')
    ap.add_argument('--boot', type=float, default=420, help='seconds to wait for a world (a cold shader cache is slow)')
    ap.add_argument('--port', type=int, default=7791)
    ap.add_argument('--env', nargs='*', default=[], help='extra VAR=value for the game (e.g. SAVAGEWIND_VK_CPU_OPTS=3314)')
    args = ap.parse_args()
    args.base_ini = args.ini
    args.fullscreen = args.potato = args.visible = False
    args.vram_mb = args.ram_mb = 0
    args.cache_reset = None
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.profiles, exist_ok=True)
    failed = []
    for config in args.configs:
        places = CONFIGS[config][2]
        for route in pr.ROUTES:
            if (args.routes and route[0] not in args.routes) or (places and route[0] not in places):
                continue
            scenes = [s for s in SCENES if config == 'play' or s[0] in SHORT]
            if args.quick:
                scenes = [s for s in SCENES if s[0] in QUICK]
            for attempt in (1, 2):  # (one retry: a boot that lost its camera or crashed)
                if one_boot(args, config, route, scenes, attempt):
                    break
            else:
                failed.append('{}/{}'.format(config, route[0]))
    print('done; boots without a profile: {}'.format(', '.join(failed) or 'none'), flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
