# PyInstaller spec: online play's RomFS overlay builder (tools/online_mod.py) as a standalone onedir helper, so a
# release makes online_mod/ from the player's own game files without Python (runtime/main.c).
# `make online-helper` -> build/helper/swonline/ (swonline[.exe] + _internal/). Same command line as online_mod.py.
import os
import sys

TOOLS = os.path.normpath(SPECPATH)

a = Analysis(
    [os.path.join(TOOLS, 'online_mod.py')],
    pathex=[TOOLS],
    binaries=[],
    datas=[],
    hiddenimports=['oead'],
    excludes=['tkinter', 'pydoc', 'doctest', 'unittest', 'pdb', 'IPython', 'numpy', 'setuptools', '_distutils_hack',
              'pkg_resources', 'ssl', '_ssl', '_hashlib', '_decimal'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    exclude_binaries=True,
    name='swonline',
    console=True,
    strip=sys.platform == 'darwin',
    upx=False,
)
coll = COLLECT(exe, a.binaries, a.datas, strip=sys.platform == 'darwin', upx=False, name='swonline')
