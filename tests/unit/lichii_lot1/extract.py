"""Exercise private production methods unchanged, with GPU dependencies replaced by fixtures."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)


def method(path, name):
    source = (root / path).read_text()
    anchor = source.index(name if '(' in name else name + '(')
    start = source.rfind('\n', 0, anchor) + 1
    brace = source.index('{', anchor)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


(output / 'demote.inc').write_text(method(
    'src/yuzu_shader_recompiler/frontend/maxwell/structured_control_flow.cpp',
    'void DemoteCombinationPass'))
source = 'src/yuzu_video_core/buffer_cache/buffer_cache.h'
names = ['BufferCache<P>::FindBuffer', 'BufferCache<P>::MarkWrittenBuffer',
         'BufferCache<P>::ObtainCPUBuffer',
         'BufferCache<P>::SynchronizeBuffer(Buffer& buffer, DAddr device_addr, u32 size,']
methods = [method(source, name) for name in names[:-1]]
# Select the overload with the upload list, rather than its small forwarding wrapper.
methods.append(method(source, names[-1]))
(output / 'find_buffer.inc').write_text(
    '\n\n'.join('template <class P>\n' + text for text in methods))
