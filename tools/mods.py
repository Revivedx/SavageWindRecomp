#!/usr/bin/env python3
"""Mods: what is in the mods/ folder is what the game plays with (docs/MODS.md).

    py -3.12 tools/mods.py          install the BNPs new in mods/, uninstall the ones gone, merge
    py -3.12 tools/mods.py list     what is installed

Players don't run this: the game runs it frozen, as swmods (tools/swmods.spec, runtime/mods.c), when mods/ changes. From
a source checkout without the helper, this script does the same with BCML (a separate program, GPLv3,
https://github.com/NiceneNerd/BCML) installed by its first run in mods/.bcml/venv. BCML installs and merges the BNPs
against the game files in extracted/. The result goes to mods/.merged/, which the game finds by itself
(runtime/services/fs.c overlay_dir); extracted/ is never changed.
"""
import base64
import hashlib
import json
import multiprocessing
import os
import shutil
import subprocess
import sys
import traceback
import types
from pathlib import Path

# (tools/mods_requirements.txt: BCML 3.8.6, its last version that is only Python, and what works with it, as wheels)
FROZEN = getattr(sys, 'frozen', False)
GAME_ID, DLC_ID = '01007EF00011E000', '01007EF00011F001'
MOD_FILES = ('.bnp', '.zip', '.rar', '.7z')
ROMFS_FOLDERS = ('Actor', 'Effect', 'Event', 'Font', 'Game', 'Layout', 'Map', 'Model', 'Movie', 'NavMesh', 'Pack', 'Physics',
                 'Sound', 'System', 'Terrain', 'UI', 'Voice')  # (the top of the game's RomFS)

# BCML's window isn't used, and it can't be installed on current Pythons. (Here, not in sync(): BCML's worker processes
# import this file again.)
if 'webview' not in sys.modules:
    webview = types.ModuleType('webview')
    webview.Window = object
    webview.windows = []
    sys.modules['webview'] = webview


def text_hashing():
    """BCML hashes text as the old xxhash did; the new one takes bytes only."""
    import xxhash

    def takes_text(digest):
        return lambda data, *args: digest(data.encode() if isinstance(data, str) else data, *args)
    xxhash.xxh64_hexdigest = takes_text(xxhash.xxh64_hexdigest)
    xxhash.xxh64_intdigest = takes_text(xxhash.xxh64_intdigest)


def file_hash(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def plan(found, state):
    """From {file: hash} in the folder and {file: {'hash', 'id'}} installed: (files to install, files to uninstall). A
    file that changed is both."""
    add = sorted(f for f, h in found.items() if f not in state or state[f]['hash'] != h)
    remove = sorted(f for f, m in state.items() if f not in found or found[f] != m['hash'])
    return add, remove


def find_sources(folder):
    """What to install from an unpacked archive: the BNPs in it, else a mod with its info.json, else the folders that
    hold a romfs (title id/romfs, as Atmosphere's contents/, or a bare romfs), else the folder with a RomFS's own
    folders loose in it. [] when it is none of these."""
    bnps = sorted(folder.rglob('*.bnp'))
    if bnps:
        return bnps
    infos = sorted(folder.rglob('info.json'), key=lambda f: len(f.parts))
    if infos:
        return [infos[0].parent]
    roots = []
    for romfs in sorted(d for d in folder.rglob('romfs') if d.is_dir()):
        title = romfs.parent
        root = title.parent if title.name.upper() in (GAME_ID, DLC_ID) else title
        if root not in roots:
            roots.append(root)
    if roots:
        return roots
    loose = sorted({d.parent for d in folder.rglob('*') if d.is_dir() and d.name in ROMFS_FOLDERS},
                   key=lambda d: len(d.parts))
    return loose[:1]


def as_mod_folder(root, name):
    """Makes a folder of a romfs into what BCML installs: the title's folder, and an info.json like a BNP's."""
    if (root / 'romfs').is_dir():
        (root / GAME_ID).mkdir(exist_ok=True)
        shutil.move(str(root / 'romfs'), str(root / GAME_ID / 'romfs'))
    elif not (root / GAME_ID).exists() and not (root / DLC_ID).exists():
        for folder in ROMFS_FOLDERS:
            if (root / folder).is_dir():
                (root / GAME_ID / 'romfs').mkdir(parents=True, exist_ok=True)
                shutil.move(str(root / folder), str(root / GAME_ID / 'romfs' / folder))
    if not (root / 'info.json').exists():
        info = {'name': name, 'image': '', 'url': '', 'desc': '', 'version': '1.0.0', 'options': {}, 'depends': [],
                'showCompare': False, 'platform': 'switch', 'id': base64.b64encode(f'{name}==1.0.0'.encode()).decode()}
        (root / 'info.json').write_text(json.dumps(info), encoding='utf-8')


def ids_of(mod):
    """The BCML ids a file in mods/ installed (an archive can hold several BNPs)."""
    return mod.get('ids') or [mod['id']]


def default_selects(info):
    """The optional parts a mod turns on by default, as BCML's install screen would (folders)."""
    opts = info.get('options') or {}
    picks = [o['folder'] for o in opts.get('multi', []) if o.get('default')]
    for group in opts.get('single', []):
        choices = group.get('options', [])
        if group.get('required') and choices:
            picks.append(choices[0]['folder'])
    return picks


class Paths:
    def __init__(self, data):
        self.data = data
        self.mods = data / 'mods'
        self.bcml = self.mods / '.bcml'
        self.venv = self.bcml / 'venv'
        self.python = self.venv / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
        self.home = self.bcml / 'data'
        self.state = self.bcml / 'state.json'
        self.work = self.bcml / 'work'
        self.merged = self.mods / '.merged'


def ensure_venv(p):
    if p.python.exists():
        return
    print('Installing BCML (only the first time)...')
    p.bcml.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, '-m', 'venv', str(p.venv)], check=True)
    pip = [str(p.python), '-m', 'pip', 'install', '-q', '--disable-pip-version-check', '--only-binary=:all:', '--no-deps']
    try:
        subprocess.run(pip + ['-r', str(Path(__file__).with_name('mods_requirements.txt'))], check=True)
    except subprocess.CalledProcessError:
        shutil.rmtree(p.venv, ignore_errors=True)
        raise


def write_settings(p):
    romfs = p.data / 'extracted' / 'romfs'
    if not (romfs / 'Pack' / 'Bootup.pack').exists():
        sys.exit(f'No game files in {romfs}: set the game up first.')
    (p.home / 'bcml').mkdir(parents=True, exist_ok=True)
    (p.bcml / 'store').mkdir(parents=True, exist_ok=True)
    aoc = p.data / 'extracted' / 'aoc1'
    settings = {
        'wiiu': False, 'no_cemu': True, 'game_dir_nx': str(romfs), 'dlc_dir_nx': str(aoc) if aoc.exists() else '',
        'store_dir': str(p.bcml / 'store'), 'no_hardlinks': False, 'suppress_update': True, 'loaded': True, 'lang': 'USen',
        'cemu_dir': '', 'game_dir': '', 'update_dir': '', 'dlc_dir': '', 'export_dir': '', 'export_dir_nx': '',
        'load_reverse': False, 'site_meta': '', 'dark_theme': False, 'no_guess': False, 'force_7z': False, 'nsfw': False,
        'auto_gb': False, 'show_gb': False, 'changelog': False, 'strip_gfx': False,
    }
    (p.home / 'bcml' / 'settings.json').write_text(json.dumps(settings), encoding='utf-8')


def seven_zip(util):
    """A 7-Zip that is installed, else BCML's own."""
    for c in (shutil.which('7z'), r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe'):
        if c and Path(c).exists():
            return str(c)
    return util.get_7z_path()


def load_state(p):
    return json.loads(p.state.read_text(encoding='utf-8')) if p.state.exists() else {}


def keep_game_file(pack, game_pack, name):
    """The pack with its file `name` as the game's own has it; None when they already match."""
    import oead
    mine, game = oead.Sarc(pack), oead.Sarc(game_pack)
    own, theirs = game.get_file(name), mine.get_file(name)
    if not own or not theirs or bytes(own.data) == bytes(theirs.data):
        return None
    writer = oead.SarcWriter.from_sarc(mine)
    writer.files[name] = bytes(own.data)
    return bytes(writer.write()[1])


def keep_game_layout(p):
    """Mods made for an older version bring its whole Bootup.pack, with that version's HUD and menus (Layout/Common):
    the game can't run with them, it looks for what the layout hasn't got. The game's own stays, whatever a mod says."""
    pack = p.merged / GAME_ID / 'romfs' / 'Pack' / 'Bootup.pack'
    if not pack.exists():
        return
    kept = keep_game_file(pack.read_bytes(), (p.data / 'extracted' / 'romfs' / 'Pack' / 'Bootup.pack').read_bytes(),
                          'Layout/Common.sblarc')
    if kept is None:
        return
    print("A mod's Layout/Common is for another version of the game: keeping the game's own.")
    pack.write_bytes(kept)


def publish(p, install, util):
    """mods/.merged/: every mod's files, the merged ones on top; add-on content where the game looks for it."""
    shutil.rmtree(p.merged, ignore_errors=True)
    if not util.get_installed_mods():
        return
    install.link_master_mod(p.merged)
    dlc = p.merged / DLC_ID / 'romfs'
    if dlc.exists():
        target = p.merged / GAME_ID / 'romfs' / 'aoc' / '0010'
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(dlc), str(target))
        shutil.rmtree(p.merged / DLC_ID, ignore_errors=True)
    keep_game_layout(p)


def install_file(path, exe, install):
    """Installs a BNP or a folder BCML can read; the mod."""
    info = json.loads((path / 'info.json').read_text('utf-8')) if path.is_dir() else install.extract_mod_meta(path)
    mod = install.install_mod(path, options={'options': {}, 'disable': []}, selects=default_selects(info))
    if not mod:
        raise ValueError(f'BCML could not read {path.name}')
    return mod


def install_any(path, work, exe, install):
    """Installs what a file in mods/ holds; its mods."""
    if path.suffix.lower() == '.bnp':
        return [install_file(path, exe, install)]
    out = work / (path.name + '.out')
    subprocess.run([exe, 'x', str(path), f'-o{out}', '-y'], check=True, stdout=subprocess.DEVNULL)
    sources = find_sources(out)
    if not sources:
        raise ValueError('no .bnp, info.json or game folders inside (a rules.txt mod is for Cemu, on Wii U)')
    mods = []
    for source in sources:
        if source.is_dir():
            as_mod_folder(source, path.stem)
        mods.append(install_file(source, exe, install))
    return mods


def sync(p):
    write_settings(p)
    text_hashing()
    import bcml.util as util
    import bcml.install as install
    exe = seven_zip(util)
    util.get_7z_path = install.get_7z_path = lambda: exe

    state = load_state(p)
    found = {f.name: file_hash(f) for f in sorted(p.mods.iterdir()) if f.is_file() and f.suffix.lower() in MOD_FILES}
    add, remove = plan(found, state)
    report = p.bcml / 'errors.txt'
    report.unlink(missing_ok=True)
    if not add and not remove and (p.merged.exists() or not state):  # (no result: the last merge failed, it goes again)
        print(f'Nothing to do, mods installed: {len(state)}.')
        return
    for name in remove:
        for mod in util.get_installed_mods(True):
            if mod.id in ids_of(state[name]):
                print(f'Removing {mod.name}...')
                install.uninstall_mod(mod, wait_merge=True)
        del state[name]
        p.state.write_text(json.dumps(state, indent=1), encoding='utf-8')
    shutil.rmtree(p.work, ignore_errors=True)
    p.work.mkdir(parents=True)
    errors = []
    for name in add:
        print(f'Adding {name}...')
        copy = p.work / name  # (BCML's install can consume what it is given)
        shutil.copy(p.mods / name, copy)
        try:  # (one bad mod doesn't stop the others; it is tried again next time)
            mods = install_any(copy, p.work, exe, install)
        except Exception as err:  # pylint: disable=broad-except
            print(f'{name} could not be installed: {err}')
            errors.append(f'{name}: {err}\n{traceback.format_exc()}')
            continue
        state[name] = {'hash': found[name], 'ids': [m.id for m in mods], 'name': ', '.join(m.name for m in mods)}
        p.state.write_text(json.dumps(state, indent=1), encoding='utf-8')
    shutil.rmtree(p.work, ignore_errors=True)
    print('Merging...')
    shutil.rmtree(p.merged, ignore_errors=True)
    try:
        if util.get_installed_mods():
            install.refresh_merges()
        publish(p, install, util)
    except Exception as err:  # pylint: disable=broad-except
        print(f'The mods could not be merged: {err}')
        errors.append(f'merging: {err}\n{traceback.format_exc()}')
    print(f'Done, mods installed: {len(state)}.')
    if errors:
        report.write_text('\n'.join(errors), encoding='utf-8')
        sys.exit(1)


def main():
    multiprocessing.freeze_support()  # (BCML's worker processes, in the frozen helper)
    args = sys.argv[1:]
    if FROZEN:  # (started by the game, in its data folder)
        data = Path.cwd()
    else:
        data = Path(os.environ.get('SAVAGEWIND_DATA_ROOT') or Path(__file__).resolve().parent.parent)
    p = Paths(data)
    p.mods.mkdir(exist_ok=True)
    if FROZEN and not args:
        os.environ['LOCALAPPDATA'] = str(p.home)  # (BCML's settings, here and in its worker processes)
        sync(p)
    elif args == ['list']:
        state = load_state(p)
        for f, m in state.items():
            print(f"{m['name']}  ({f})")
        print(f'Mods installed: {len(state)}.')
    elif args:
        sys.exit(__doc__)
    elif Path(sys.prefix).resolve() == p.venv.resolve():
        sync(p)
    else:
        ensure_venv(p)
        env = dict(os.environ, LOCALAPPDATA=str(p.home), PYTHONIOENCODING='utf-8')
        sys.exit(subprocess.run([str(p.python), '-I', os.path.abspath(__file__)], env=env).returncode)


if __name__ == '__main__':
    main()
