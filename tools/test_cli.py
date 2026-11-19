#!/usr/bin/env python3
"""savagewind <command> (runtime/cli.c, docs/cli.md): every command against a scratch data root, no game files needed.

Checks exit statuses, that every --json output parses, and the commands that change files: settings set (taken and
refused), launch options, save profiles, and a backup restored without losing the saves it replaces (a synthetic
save folder). Never touches the player's own data root: everything is in build/test_cli/, removed at the end.

usage: tools/test_cli.py [path to savagewind]      (make test-cli)
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'savagewind' + ('.exe' if os.name == 'nt' else ''))
DATA = os.path.join(ROOT, 'build', 'test_cli')
failures = []


def run(*args, code=None):
    env = dict(os.environ, SAVAGEWIND_DATA_ROOT=DATA, SAVAGEWIND_NO_UPDATE_CHECK='1')
    env.pop('SAVAGEWIND_SETTINGS', None)
    env.pop('SAVAGEWIND_SAVE_DIR', None)
    p = subprocess.run([EXE, *args], capture_output=True, text=True, env=env, timeout=120)
    if code is not None and p.returncode != code:
        failures.append(f'savagewind {" ".join(args)}: exit {p.returncode}, wanted {code}\n{p.stdout}')
    return p


def check(what, ok, detail=''):
    if not ok:
        failures.append(f'{what} {detail}'.strip())


def as_json(*args, code=0):
    out = run(*args, '--json', code=code).stdout
    try:
        return [json.loads(line) for line in out.splitlines() if line.strip()]
    except ValueError:
        failures.append(f'savagewind {" ".join(args)} --json: not JSON:\n{out}')
        return [None]


def main():
    if not os.path.isfile(EXE):
        sys.exit(f'{EXE} not built')
    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(DATA)
    try:
        tests()
    finally:
        shutil.rmtree(DATA, ignore_errors=True)
    for f in failures:
        print('FAIL', f)
    print(f'test_cli: {"FAILED" if failures else "ok"} ({len(failures)} failure(s))')
    sys.exit(1 if failures else 0)


def tests():
    # help, version, completion
    check('help lists the commands', 'settings' in run('help', code=0).stdout)
    check('help <command>', 'restore' in run('help', 'saves', code=0).stdout)
    run('help', 'nosuch', code=2)
    check('version', run('version', code=0).stdout.startswith('Savage Wind '))
    for shell in ('bash', 'zsh', 'fish'):
        check(f'completion {shell}', 'savagewind' in run('completion', shell, code=0).stdout)
    run('completion', 'tcsh', code=2)

    # an empty data root: nothing ready, nothing to delete
    st = as_json('status', code=1)[0]
    check('status --json on an empty root', st and st['ready'] is False and st['dlc'] == 'none', st)
    v = as_json('verify', code=1)[0]
    check('verify --json on an empty root', v and v['ok'] is False, v)
    run('run', code=1)
    run('delete', code=2)
    run('delete', '--yes', code=0)
    run('setup', os.path.join(DATA, 'no such folder'), code=2)

    # paths
    p = as_json('paths')[0]
    check('paths --json', p and os.path.samefile(p['data_root'], DATA), p)

    # settings: list, get, set (taken, refused, unknown)
    s = as_json('settings', 'list')[0]
    check('settings list --json', s and 'frame_rate' in s and 'vsync' in s, s and len(s))
    run('settings', 'set', 'volume', '0.5', code=0)
    check('settings set volume', run('settings', 'get', 'volume', code=0).stdout.strip() == '0.50')
    ini = os.path.join(DATA, 'savagewind.ini')
    before = open(ini).read()
    run('settings', 'set', 'vsync', 'banana', code=1)
    check('a refused value leaves the file as it was', open(ini).read() == before)
    run('settings', 'set', 'nosuch', '1', code=1)
    run('settings', 'get', 'nosuch', code=1)

    # launch options
    run('launch-options', 'set', 'SAVAGEWIND_FPS_CAP', '90', code=0)
    run('launch-options', 'set', 'SAVAGEWIND_DATA_ROOT', '/x', code=1)
    lo = as_json('launch-options', 'list')[0]
    check('launch-options list --json', lo == [{'name': 'SAVAGEWIND_FPS_CAP', 'value': '90', 'skipped': False}], lo)
    check('a command after a launch option is saved still prints', 'SAVAGEWIND_FPS_CAP=90' in run('launch-options', code=0).stdout)
    av = as_json('launch-options', 'available')[0]
    check('launch-options available --json', av and all(f['group'] in ('play', 'other') for f in av), len(av or []))
    run('launch-options', 'remove', 'SAVAGEWIND_FPS_CAP', code=0)
    run('launch-options', 'remove', 'SAVAGEWIND_FPS_CAP', code=1)
    run('launch-options', 'clear', code=0)

    # saves: profiles, a backup, and a restore that keeps the saves it replaces
    run('saves', 'create', 'Test run', code=0)
    run('saves', 'create', 'bad/name', code=1)
    run('saves', 'use', 'Test run', code=0)
    prof = as_json('saves', 'list')[0]
    check('saves list --json', prof and any(x['name'] == 'Test run' and x['active'] for x in prof), prof)
    run('saves', 'use', 'Default', code=0)
    slot = os.path.join(DATA, 'save', '0')
    os.makedirs(slot)
    with open(os.path.join(slot, 'game_data.sav'), 'wb') as f:
        f.write(os.urandom(1000))
    with open(os.path.join(slot, 'marker'), 'w') as f:
        f.write('original')
    run('saves', 'backup', code=0)
    backups = as_json('saves', 'backups')[0]
    check('one backup', backups and len(backups) == 1, backups)
    with open(os.path.join(slot, 'marker'), 'w') as f:
        f.write('changed')
    run('saves', 'restore', backups[0], code=2)
    run('saves', 'restore', '../save', '--yes', code=1)
    run('saves', 'restore', backups[0], '--yes', code=0)
    check('restore put the backup back', open(os.path.join(slot, 'marker')).read() == 'original')
    after = as_json('saves', 'backups')[0]
    check('restore kept the replaced saves as a backup', after and len(after) == 2, after)
    if after and len(after) == 2:
        kept = os.path.join(DATA, 'save-backups', after[1], '0', 'marker')
        check('the replaced saves', os.path.isfile(kept) and open(kept).read() == 'changed')

    # shaders, log
    sh = as_json('shaders')[0]
    check('shaders --json', sh and 'available' in sh, sh)
    check('log', '[savagewind]' in run('log', code=0).stdout)


if __name__ == '__main__':
    main()
