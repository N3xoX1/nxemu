#!/usr/bin/env python3
"""Vendor the unmodified Intel CMAA2 shader pinned by its Git blob SHA-1.

This script is never called by normal emulator runtime. Requires network once.
Original source: https://github.com/GameTechDev/CMAA2
License: Apache-2.0; copyright (c) 2018 Intel Corporation.
"""
import argparse
import hashlib
import pathlib
import urllib.request

URL = 'https://raw.githubusercontent.com/GameTechDev/CMAA2/master/Projects/CMAA2/CMAA2/CMAA2.hlsl'
GIT_BLOB_SHA1 = '717101c85ef6e933dee3d193f514410b51d09c45'
LICENSE_URL = 'https://raw.githubusercontent.com/GameTechDev/CMAA2/master/license.txt'
LICENSE_BLOB_SHA1 = 'e71036ef65967bc780427b94313459f9da2d8981'

def verify(payload: bytes) -> str:
    digest = hashlib.sha1(f'blob {len(payload)}\0'.encode() + payload).hexdigest()
    if digest != GIT_BLOB_SHA1:
        raise ValueError(f'Intel CMAA2 source changed: expected {GIT_BLOB_SHA1}, got {digest}')
    return digest

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('destination', type=pathlib.Path)
    args=parser.parse_args()
    with urllib.request.urlopen(URL, timeout=30) as resp:
        data=resp.read()
    print('Verified Intel CMAA2 source blob', verify(data))
    with urllib.request.urlopen(LICENSE_URL, timeout=30) as resp:
        license_bytes = resp.read()
    license_hash = hashlib.sha1(f'blob {len(license_bytes)}\0'.encode() + license_bytes).hexdigest()
    if license_hash != LICENSE_BLOB_SHA1:
        raise ValueError(f'Unexpected Intel Apache license revision: {license_hash}')
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(data)
    (args.destination.parent / 'license.txt').write_bytes(license_bytes)

if __name__ == '__main__':
    main()
