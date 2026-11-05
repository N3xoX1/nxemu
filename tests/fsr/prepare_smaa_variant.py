#!/usr/bin/env python3
"""Compile a test-only preset of the production SMAA shader, leaving it unchanged."""
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('header', type=Path)
parser.add_argument('preset', choices=('high', 'medium', 'ultra'))
parser.add_argument('name')
parser.add_argument('glslang')
parser.add_argument('validator')
parser.add_argument('--zero-edges', action='store_true')
args = parser.parse_args()
text = args.source.read_text(encoding='utf-8')
needle = '#define SMAA_PRESET_ULTRA'
if text.count(needle) != 1:
    raise SystemExit('Unexpected production SMAA preset declaration')
text = text.replace(needle, '#define SMAA_PRESET_' + args.preset.upper())
args.header.parent.mkdir(parents=True, exist_ok=True)
if args.zero_edges:
    library = (args.source.parent / 'opengl_smaa.glsl').read_text(encoding='utf-8')
    # These are the three vec2 edge-detection functions. No stencil mask is used
    # by this renderer; writing zero prevents stale edges without a clear pass.
    if library.count('        discard;') != 3:
        raise SystemExit('Unexpected SMAA edge discard sites')
    library = library.replace('        discard;', '        return float2(0.0, 0.0);')
    (args.header.parent / 'smaa_zero_edges.glsl').write_text(library, encoding='utf-8')
    text = text.replace('#include "opengl_smaa.glsl"', '#include "smaa_zero_edges.glsl"')
prepared = args.header.with_suffix(args.source.suffix)
prepared.write_text(text, encoding='utf-8')
spirv = args.header.with_suffix('.spv')
subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.1',
                '-I' + str(args.source.parent), '-o', str(spirv), str(prepared)], check=True)
subprocess.run([args.validator, '--target-env', 'vulkan1.1', str(spirv)], check=True)
root = Path(__file__).resolve().parents[2]
subprocess.run([sys.executable, str(root / 'tools/cmaa2/spirv_to_header.py'),
                str(spirv), str(args.header), args.name], check=True)
