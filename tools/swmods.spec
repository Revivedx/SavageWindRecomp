# PyInstaller spec: tools/mods.py with BCML as a standalone onedir helper, so the game can apply mods/ on the player's
# machine without Python (runtime/mods.c). `make mods-helper` -> build/helper/swmods/ (swmods[.exe] + _internal/).
# BCML is GPLv3 and stays a separate program: docs/MODS.md.
import glob
import importlib.util
import os

from PyInstaller.utils.hooks import collect_data_files, collect_submodules

TOOLS = os.path.normpath(SPECPATH)

# BCML's modules by file: importing them to list them needs its window, which isn't here
BCML_DIR = list(importlib.util.find_spec('bcml').submodule_search_locations)[0]
BCML_UI = {'__main__', '_api', '_oneclick', '_server', 'gamebanana'}
BCML_MODULES = ['bcml.' + n for n in (os.path.basename(f)[:-3] for f in glob.glob(BCML_DIR + '/*.py')) if n not in BCML_UI]
BCML_MODULES += ['bcml.mergers.' + os.path.basename(f)[:-3] for f in glob.glob(BCML_DIR + '/mergers/*.py')]

a = Analysis(
    [os.path.join(TOOLS, 'mods.py')],
    pathex=[TOOLS],
    binaries=[],
    # (BCML's hashes and helpers, and its libraries' tables, found next to their modules)
    datas=sum((collect_data_files(p) for p in ('bcml', 'aamp', 'byml', 'botw', 'botw_havok', 'oead', 'rstb')), []),
    hiddenimports=BCML_MODULES + collect_submodules('botw_havok'),
    excludes=['tkinter', 'pydoc', 'doctest', 'unittest', 'pdb', 'IPython', 'webview', 'setuptools', '_distutils_hack',
              'pkg_resources'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, exclude_binaries=True, name='swmods', console=True, upx=False)
coll = COLLECT(exe, a.binaries, a.datas, upx=False, name='swmods')
