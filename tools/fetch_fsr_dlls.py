#!/usr/bin/env python3
"""Downloads AMD's FSR DLLs for Vulkan (FidelityFX SDK v2.4.0, signedbin) that hold Video > Upscaler "AMD FSR 4".

AMD ships FSR 4 only as signed Windows x64 DLLs (no Linux or macOS build). runtime/nvn/vulkan/fsr_api.cpp loads
amd_fidelityfx_loader_vk.dll from the game's folder if it is there; without it the game is as before and FSR 4 is
listed as AMD Radeon only. The DLLs are not in this repository (36 MB): this fetches them, checks their SHA-256 and
writes AMD's licence text (also checked) next to them (its terms: binary redistribution is fine with that notice reproduced; no
reverse engineering).

usage: tools/fetch_fsr_dlls.py                 into build/fsr-dlls/ (make dist copies them from there when present)
       tools/fetch_fsr_dlls.py --dest DIR      into DIR instead, e.g. the folder of a built savagewind.exe
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request

TAG = 'v2.4.0'
BASE = f'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{TAG}/Kits/FidelityFX'
FILES = {
    'signedbin/amd_fidelityfx_loader_vk.dll': ('amd_fidelityfx_loader_vk.dll',
        '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
    'signedbin/amd_fidelityfx_upscaler_vk.dll': ('amd_fidelityfx_upscaler_vk.dll',
        '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'),
    'docs/license.md': ('AMD-FidelityFX-DLL-LICENSE.txt',
        '32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb'),
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--dest', default=os.path.join(root, 'build', 'fsr-dlls'))
    dest = ap.parse_args().dest
    os.makedirs(dest, exist_ok=True)
    for src, (name, want) in FILES.items():
        out = os.path.join(dest, name)
        if os.path.exists(out) and sha256(out) == want:
            print(f'{name}: already there')
            continue
        print(f'{name}: downloading {src} ({TAG})')
        tmp = out + '.part'
        try:
            with urllib.request.urlopen(f'{BASE}/{src}', timeout=60) as r, open(tmp, 'wb') as f:
                shutil.copyfileobj(r, f)
            if sha256(tmp) != want:
                sys.exit(f'{name}: SHA-256 differs from the one recorded for {TAG}; not kept')
            os.replace(tmp, out)
        finally:
            if os.path.exists(tmp):
                os.remove(tmp)
    print(f'done: {dest}')


if __name__ == '__main__':
    main()
