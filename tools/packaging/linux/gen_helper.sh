#!/bin/bash
# tools/packaging/linux/gen_helper.sh <x86_64|aarch64> (inside the build container of that arch, tools/packaging/linux/appimage.sh): the
# game code generator frozen by PyInstaller (tools/recomp/swgen.spec, as `make gen-helper`) with Ubuntu 22.04's Python 3.12,
# into build/linux/<arch>/helper/swgen.
set -euo pipefail
cd "$(dirname "$0")/../../.."
L=$PWD/build/linux/$1
V=$L/gen-venv
if [ ! -f "$V/.ok" ]; then
    rm -rf "$V" && python3.12 -m venv "$V"
    "$V/bin/python" -m pip install -q --upgrade pip
    "$V/bin/python" -m pip install -q capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5 pyinstaller==6.22.3
    touch "$V/.ok"
fi
"$V/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" --workpath "$L/pyinstaller" \
    tools/recomp/swgen.spec
du -sh "$L/helper/swgen"
