"""Compile production vertex-input methods with a recording Vulkan fixture."""
from pathlib import Path
import sys

def method(source, name):
    anchor = source.index(name + '(')
    start = source.rfind('\n', 0, anchor) + 1
    brace = source.index('{', anchor)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

def main():
    root, output = map(Path, sys.argv[1:3])
    output.mkdir(parents=True, exist_ok=True)

    def read(path):
        return (root / 'src/yuzu_video_core' / path).read_text()

    source = read('renderer_vulkan/vk_rasterizer.cpp')
    (output / 'update.inc').write_text('\n\n'.join(method(source, 'RasterizerVulkan::' + name)
        for name in ['UpdateVertexInput', 'RefreshVertexInput']))
    (output / 'notify.inc').write_text(method(read('renderer_vulkan/vk_buffer_cache.cpp'),
                                            'BufferCacheRuntime::NotifyVertexBufferStride'))
    (output / 'format.inc').write_text(method(read('renderer_vulkan/maxwell_to_vk.cpp'),
                                            'VertexFormat'))
    source = read('renderer_vulkan/vk_graphics_pipeline.cpp')
    start = source.index('    static_vector<VkDynamicState, 28> dynamic_states{')
    end = source.index('    const VkPipelineDynamicStateCreateInfo', start)
    (output / 'states.inc').write_text(source[start:end])

if __name__ == '__main__':
    main()
