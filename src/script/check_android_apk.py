#!/usr/bin/env python3
"""Check packaged native modules, ELF architecture/alignment and dependencies."""
import argparse
import struct
import zipfile
from pathlib import Path

EXPECTED = {f"libnxemu-{name}.so" for name in ("android", "cpu", "loader", "os", "video")}
SYSTEM = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
    "libvulkan.so", "libmediandk.so", "libOpenSLES.so", "libEGL.so",
    "libGLESv2.so", "libGLESv3.so", "libjnigraphics.so", "libaaudio.so",
}


def check(apk: Path) -> None:
    with zipfile.ZipFile(apk) as archive:
        libraries = {
            Path(name).name: archive.read(name)
            for name in archive.namelist()
            if name.startswith("lib/arm64-v8a/") and name.endswith(".so")
        }
    missing = EXPECTED - libraries.keys()
    if missing:
        raise ValueError(f"Missing native modules: {sorted(missing)}")
    for name, data in sorted(libraries.items()):
        if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
            raise ValueError(f"{name}: expected a little-endian AArch64 ELF64 library")
        phoff = struct.unpack_from("<Q", data, 32)[0]
        phsize, phnum = struct.unpack_from("<HH", data, 54)
        headers = [struct.unpack_from("<IIQQQQQQ", data, phoff + i * phsize) for i in range(phnum)]
        loads = [header for header in headers if header[0] == 1]
        if not loads or any(header[7] < 16384 or (header[2] - header[3]) % 16384 for header in loads):
            raise ValueError(f"{name}: LOAD segments are not aligned for 16 KiB host pages")
        needed = []
        strings = None
        for header in headers:
            if header[0] == 2:
                for offset in range(header[2], header[2] + header[5], 16):
                    tag, value = struct.unpack_from("<qQ", data, offset)
                    if tag == 0:
                        break
                    if tag == 1:
                        needed.append(value)
                    elif tag == 5:
                        strings = value
        if needed and strings is None:
            raise ValueError(f"{name}: missing dynamic string table")
        if strings is not None:
            mapping = next((h for h in loads if h[3] <= strings < h[3] + h[5]), None)
            if mapping is None:
                raise ValueError(f"{name}: unmapped dynamic string table")
            base = mapping[2] + strings - mapping[3]
            for index in needed:
                start = base + index
                dependency = data[start:data.index(b"\0", start)].decode("ascii")
                if dependency not in libraries and dependency not in SYSTEM:
                    raise ValueError(f"{name}: dependency {dependency} is not packaged or an Android system library")
        print(f"{name}: AArch64, 16 KiB ELF alignment, dependencies present")
    print(f"Validated {apk}: {len(libraries)} native libraries")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    check(parser.parse_args().apk)
