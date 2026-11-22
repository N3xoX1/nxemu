"""Compile production command processing bodies against deterministic host fixtures."""
from pathlib import Path
import sys
root, output = map(Path, sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)
def method(source, name):
    anchor = source.index(name + '(')
    start = source.rfind('\n', 0, anchor) + 1
    brace = source.index('{', anchor)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
s = (root / 'src/yuzu_video_core/engines/maxwell_3d.cpp').read_text()
names = ['IsMethodExecutable', 'ProcessShadowRam', 'ProcessDirtyRegisters',
         'PushMethod', 'PushMethodRun', 'ConsumeSinkImpl', 'CallMethod', 'ProcessMethodCall']
(output / 'engine.inc').write_text('\n\n'.join(method(s, 'Maxwell3D::'+n).replace('Maxwell3D::','Fixture::') for n in names))
names = ['ProcessCommands', 'SetState', 'CallMethod', 'CallMultiMethod']
s = (root / 'src/yuzu_video_core/dma_pusher.cpp').read_text()
(output / 'dma.inc').write_text('\n\n'.join(method(s, 'DmaPusher::'+n).replace('DmaPusher::','Dma::') for n in names))
(output / 'baseline.inc').write_text((root / 'tests/unit/gpu_commands/baseline.inc').read_text())
