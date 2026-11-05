#!/usr/bin/env python3
"""Turn DXC-generated SPIR-V into a C++ uint32_t array (as required by NXEmu)."""
import pathlib
import struct
import sys

def convert(source: pathlib.Path, dest: pathlib.Path, name: str):
    blob = source.read_bytes()
    if len(blob) % 4 or len(blob) < 20:
        raise ValueError('Invalid SPIR-V size')
    words = struct.unpack('<'+'I'*(len(blob)//4), blob)
    if words[0] != 0x07230203:
        raise ValueError('Invalid SPIR-V magic')
    lines=['#pragma once', '#include <cstdint>', f'inline constexpr uint32_t {name}[] = {{']
    for start in range(0,len(words),8):
        lines.append('    '+', '.join(f'0x{n:08x}u' for n in words[start:start+8])+',')
    lines+=['};','']
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text('\n'.join(lines))

if __name__=='__main__':
    convert(pathlib.Path(sys.argv[1]),pathlib.Path(sys.argv[2]),sys.argv[3])
