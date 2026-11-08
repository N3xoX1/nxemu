"""Compile the unchanged private control-flow methods with host-only fixtures."""
from pathlib import Path
import sys
root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)


def definition(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


source = (root / 'src/yuzu_shader_recompiler/frontend/maxwell/control_flow.cpp').read_text()
compare = definition(source, 'struct Compare') + ';'
method = definition(source, 'bool CFG::InspectVisitedBlocks').replace('CFG::', 'Fixture::')
(output / 'visited_blocks.inc').write_text(compare + '\n\n' + method)
source = (root / 'src/yuzu_shader_recompiler/frontend/maxwell/structured_control_flow.cpp').read_text()
(output / 'ordered.inc').write_text(definition(source, 'bool AreOrdered'))
