#!/usr/bin/env python3
"""P0 build gate: compile actual Intel shaders, validate SPIR-V and reflect descriptors.

Run on a workstation/CI with DXC, glslangValidator and spirv-val. The script
intentionally exits nonzero when tools or pinned upstream Intel source are missing.
It does not execute a Vulkan renderer or measure performance.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'src/yuzu_video_core/host_shaders'
TOOLS = ROOT / 'tools/cmaa2'
SOURCE = ROOT / 'src/3rd_party/intel_cmaa2/CMAA2.hlsl'


def run(*args):
    print('+', ' '.join(str(arg) for arg in args), flush=True)
    subprocess.run(list(map(str, args)), check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--intel-source', type=Path, default=SOURCE)
    arguments = parser.parse_args()
    tool = {}
    for name in ('dxc', 'glslangValidator', 'spirv-val'):
        tool[name] = shutil.which(name)
        if not tool[name]:
            print('BLOCKED: required shader tool not found:', name, file=sys.stderr)
            return 2
    if not arguments.intel_source.is_file():
        print('BLOCKED: pinned Intel source absent:', arguments.intel_source, file=sys.stderr)
        print('Run python3 tools/cmaa2/fetch_intel_cmaa2.py ' + str(SOURCE), file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory(prefix='nxemu-cmaa2-p0-') as folder:
        folder = Path(folder)
        prepared = folder / 'cmaa2.hlsl'
        run(sys.executable, TOOLS / 'prepare_intel_shader.py', arguments.intel_source, prepared)
        outputs = {}
        for stage, entry in (('edges', 'EdgesColor2x2CS'),
                             ('args', 'ComputeDispatchArgsCS'),
                             ('candidates', 'ProcessCandidatesCS')):
            target = folder / f'{stage}.spv'
            run(tool['dxc'], '-spirv', '-T', 'cs_6_0', '-E', entry,
                '-fspv-entrypoint-name=main', '-fspv-target-env=vulkan1.1',
                '-fvk-u-shift', '0', '0', '-fvk-t-shift', '8', '0',
                '-fvk-s-shift', '12', '0',
                '-DCMAA2_UAV_STORE_TYPED=1',
                '-DCMAA2_UAV_STORE_TYPED_UNORM_FLOAT=0',
                '-DCMAA2_UAV_STORE_CONVERT_TO_SRGB=0',
                '-DCMAA2_SUPPORT_HDR_COLOR_RANGE=1',
                '-DCMAA2_EDGE_DETECTION_LUMA_PATH=1',
                '-DCMAA2_STATIC_QUALITY_PRESET=1',
                '-DCMAA2_USE_HALF_FLOAT_PRECISION=0',
                '-Fo', target, prepared)
            run(tool['spirv-val'], '--target-env', 'vulkan1.1', target)
            outputs[stage] = target
        for stage in ('vert', 'frag'):
            output = folder / f'{stage}.spv'
            run(tool['glslangValidator'], '-V', '-S', stage, '--target-env', 'vulkan1.1',
                '-o', output, SHADERS / f'intel_cmaa2_apply.{stage}')
            run(tool['spirv-val'], '--target-env', 'vulkan1.1', output)
            outputs[stage] = output
        run(sys.executable, TOOLS / 'verify_spirv.py',
            *sum(([f'--{stage}', outputs[stage]] for stage in
                  ('edges', 'args', 'candidates', 'vert', 'frag')), []))
    print('PASS: P0 shader compile, spirv-val and resource reflection; Vulkan GPU execution NOT tested')
    return 0


if __name__ == '__main__':
    sys.exit(main())
