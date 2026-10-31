"""Extract unmodified production methods for host-only control-flow tests.

The fixtures replace Vulkan/device memory and unrelated engine operations, so these
regressions can run without a GPU. No rendering implementation is copied into tests.
"""
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

source = (root / 'src/yuzu_video_core/engines/maxwell_3d.cpp').read_text()
engine = [method(source, 'Maxwell3D::' + name).replace('Maxwell3D::', 'Fixture::')
          for name in ['IsMethodExecutable', 'ProcessShadowRam', 'ProcessMethodCall',
                       'ProcessQueryCondition', 'CallMethod']]
source = (root / 'src/yuzu_video_core/dma_pusher.cpp').read_text()
engine.append(method(source, 'DmaPusher::CallMethod').replace('DmaPusher::', 'Dma::'))
(output / 'engine.inc').write_text('\n\n'.join(engine))
source = (root / 'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp').read_text()
names = ['EndHostConditionalRendering', 'PauseHostConditionalRendering',
         'ResumeHostConditionalRendering', 'HostConditionalRenderingCompareValueImpl',
         'HostConditionalRenderingCompareBCImpl', 'HostConditionalRenderingCompareValue',
         'HostConditionalRenderingCompareValues']
(output / 'runtime.inc').write_text('\n\n'.join(method(source, 'QueryCacheRuntime::' + n)
                                             for n in names))
