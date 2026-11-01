"""Prepare original and supplied-patch methods for the optional host benchmark.

Usage: prepare_compare.py ROOT OUTPUT PATCH [BASELINE_REF]
Patch contents are data: only the reviewed rasterizer/header hunks are used.
"""
from pathlib import Path
import re
import subprocess
import sys

root, output, patch = map(Path, sys.argv[1:4])
baseline = sys.argv[4] if len(sys.argv) > 4 else 'HEAD'
output.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(Path(__file__).parent))
from extract import method

def original(path):
    return subprocess.check_output(['git', '-C', str(root), 'show', f'{baseline}:{path}'],
                                   text=True)

def apply(source, path):
    lines = patch.read_text().splitlines(keepends=True)
    start = next(i for i, line in enumerate(lines) if line.rstrip() == '+++ b/' + path) + 1
    end = next((i for i in range(start, len(lines)) if lines[i].startswith('--- a/')), len(lines))
    result, old, cursor = [], source.splitlines(keepends=True), 0
    i = start
    while i < end:
        match = re.match(r'@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@', lines[i])
        if not match:
            i += 1
            continue
        position = int(match[1]) - 1
        result.extend(old[cursor:position])
        cursor = position
        i += 1
        while i < end and not lines[i].startswith('@@'):
            line = lines[i]
            if line[:1] in (' ', '-'):
                if old[cursor].rstrip('\r\n') != line[1:].rstrip('\r\n'):
                    raise ValueError(f'Patch context mismatch at {path}:{cursor+1}')
                cursor += 1
            if line[:1] in (' ', '+'):
                result.append(line[1:])
            i += 1
    result.extend(old[cursor:])
    return ''.join(result)

path = 'src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp'
source = original(path)
name = 'RasterizerVulkan::UpdateVertexInput'
(output / 'original.inc').write_text(method(source, name).replace(name, 'RasterizerVulkan::Original'))
supplied = method(apply(source, path), name).replace(name, 'RasterizerVulkan::Supplied')
supplied = supplied.replace('ConsumeVertexInputRefresh()', 'ConsumeSuppliedVertexInputRefresh()')
(output / 'supplied.inc').write_text(supplied)
path = 'src/yuzu_video_core/renderer_vulkan/vk_buffer_cache.h'
consume = method(apply(original(path), path), 'ConsumeVertexInputRefresh')
(output / 'consume_supplied.inc').write_text(consume.replace('ConsumeVertexInputRefresh',
                                                          'ConsumeSuppliedVertexInputRefresh'))
path = 'src/yuzu_video_core/renderer_vulkan/vk_buffer_cache.cpp'
notify = method(apply(original(path), path), 'BufferCacheRuntime::NotifyVertexBufferStride')
(output / 'notify_supplied.inc').write_text(notify.replace('NotifyVertexBufferStride',
                                                        'NotifySuppliedVertexBufferStride'))
