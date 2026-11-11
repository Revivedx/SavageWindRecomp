#!/usr/bin/env python3
"""Writes <release folder>/sw-manifest.txt: every file of a release with its size and SHA-256 (runtime/update_core.h).

SW Update checks the files of an update against it, and removes the files of the old version that the new one no
longer has only if the old version's list names them, so nothing a player keeps in the folder is ever touched.

usage: tools/make_update_manifest.py <release folder> <version>
"""
import hashlib
import os
import sys


def main():
    folder, version = sys.argv[1], sys.argv[2]
    lines = ['savagewind-update-manifest 1', f'version {version}']
    count = 0
    for base, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            rel = os.path.relpath(path, folder).replace(os.sep, '/')
            if rel == 'sw-manifest.txt':
                continue
            digest = hashlib.sha256()
            with open(path, 'rb') as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    digest.update(chunk)
            lines.append(f'f {digest.hexdigest()} {os.path.getsize(path)} {rel}')
            count += 1
    with open(os.path.join(folder, 'sw-manifest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'sw-manifest.txt: {count} files, version {version}')


if __name__ == '__main__':
    main()
