#!/usr/bin/env python3
"""Signs a Windows release zip for SW Update: writes <zip>.sig, to upload to the release next to the zip.

The key is ECDSA P-256 in a PEM file kept outside the repository; its public half is compiled into SW Update
(runtime/platform/sw_update_key.h). A release without a matching .sig is not offered to players. Needs openssl.

usage: tools/sign_update.py <zip> <private key .pem>
       tools/sign_update.py --public <private key .pem>     print the public key as hex (X then Y)
"""
import subprocess
import sys


def der_int(data, at):
    if data[at] != 2:
        raise ValueError('not a DER integer')
    size = data[at + 1]
    return int.from_bytes(data[at + 2:at + 2 + size], 'big'), at + 2 + size


def main():
    if sys.argv[1] == '--public':
        der = subprocess.check_output(['openssl', 'ec', '-in', sys.argv[2], '-pubout', '-outform', 'DER'],
                                      stderr=subprocess.DEVNULL)
        print(der[-64:].hex())
        return
    zip_path, key = sys.argv[1], sys.argv[2]
    der = subprocess.check_output(['openssl', 'dgst', '-sha256', '-sign', key, zip_path])
    if der[0] != 0x30:
        raise SystemExit('unexpected signature format')
    at = 3 if der[1] & 0x80 else 2
    r, at = der_int(der, at)
    s, at = der_int(der, at)
    with open(zip_path + '.sig', 'w', newline='\n') as f:
        f.write((r.to_bytes(32, 'big') + s.to_bytes(32, 'big')).hex() + '\n')
    print(zip_path + '.sig')


if __name__ == '__main__':
    main()
