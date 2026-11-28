"""Extract the unchanged CPU memory span helpers from nxemu-os/core/memory.cpp."""
from pathlib import Path
import re
import sys

# Definitions, in source order of dependency. Each pattern matches the start of a definition.
DEFINITIONS = [
    ('free.inc', r'bool AddressSpaceContains\('),
    ('impl.inc', r'u8\s*\*\s*GetPointerFromRasterizerCachedMemory\(u64 vaddr\) const'),
    ('impl.inc', r'u8\s*\*\s*GetPointerFromDebugMemory\(u64 vaddr\) const'),
    ('impl.inc', r'void MapPages\(Common::PageTable\s*&\s*page_table'),
    ('impl.inc', r'u8\s*\*\s*GetPointerImpl\(u64 vaddr'),
    ('impl.inc', r'u8\s*\*\s*GetPointerSilent\(const Common::ProcessAddress vaddr\) const'),
    ('impl.inc', r'u8\s*\*\s*GetSpan\(const VAddr src_addr, const std::size_t size\) const'),
]


def definition(source, pattern):
    match = re.search(pattern, source)
    if match is None:
        sys.exit(f'memory_span/extract.py: no definition matches {pattern!r}')
    start = source.rfind('\n', 0, match.start()) + 1
    brace = source.index('{', match.end())
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    root, output = map(Path, sys.argv[1:3])
    output.mkdir(parents=True, exist_ok=True)
    source = (root / 'src/nxemu-os/core/memory.cpp').read_text()
    parts = {}
    for name, pattern in DEFINITIONS:
        parts.setdefault(name, []).append(definition(source, pattern))
    for name, bodies in parts.items():
        text = '\n\n'.join(bodies) + '\n'
        path = output / name
        if not path.exists() or path.read_text() != text:
            path.write_text(text)


if __name__ == '__main__':
    main()
