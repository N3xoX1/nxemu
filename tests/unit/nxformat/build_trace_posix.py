"""Build the production NXEmu VFS trace on Linux, without CPU/GPU modules.

Requires C++20, libzstd-dev. Uses real VFS/filesystem sources
and dead-section removal; no loader or filesystem stubs.
"""
import argparse
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--nxformat', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    if not os.sys.platform.startswith('linux'):
        parser.error('this minimal link recipe is validated on Linux only')
    compiler = shutil.which(os.environ.get('CXX', 'c++'))
    if not compiler:
        parser.error('C++20 compiler required')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    common = [compiler, '-std=c++20', '-O2', '-ffunction-sections', '-fdata-sections',
              '-DFMT_HEADER_ONLY=1',
              f'-I{ROOT / "src/nxemu-loader"}', f'-I{ROOT / "src"}',
              f'-I{ROOT / "external/zstd/lib"}', f'-I{ROOT / "external/boost"}',
              f'-I{ROOT / "external/fmt/include"}']
    sources = ['src/common/sha256.cpp',
               'src/nxemu-loader/core/file_sys/vfs/vfs_nx.cpp',
               'src/nxemu-loader/core/file_sys/vfs/vfs.cpp',
               'src/yuzu_common/fs/fs_util.cpp', 'src/yuzu_common/fs/path_util.cpp']
    objects = []
    for source in sources:
        target = args.output_dir / (Path(source).stem + '.o')
        subprocess.run([*common, '-c', str(ROOT / source), '-o', str(target)], check=True)
        objects.append(str(target))
    executable = args.output_dir / 'nxemu_trace'
    subprocess.run([*common, '-DNX_BENCH_NXEMU=1', f'-I{args.nxformat / "dist"}',
                    f'-I{args.nxformat / "tests/support"}',
                    str(args.nxformat / 'benchmarks/session_trace_benchmark.cpp'), *objects,
                    '-Wl,--gc-sections', '-lzstd', '-ldl', '-pthread',
                    '-o', str(executable)], check=True)
    print(executable)


if __name__ == '__main__':
    main()
