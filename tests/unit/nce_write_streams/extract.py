"""Exercise the production BufferCache::WriteMemoryAhead unchanged, against a fixture cache."""
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)


def method(path, name):
    source = (root / path).read_text()
    anchor = source.index(name)
    start = source.rfind('\n', 0, anchor) + 1
    brace = source.index('{', anchor)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


(output / 'write_memory_ahead.inc').write_text(
    'template <class P>\n' + method('src/yuzu_video_core/buffer_cache/buffer_cache.h',
                                    'BufferCache<P>::WriteMemoryAhead('))
