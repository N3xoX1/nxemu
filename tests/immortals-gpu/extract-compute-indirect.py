"""Reuse the existing Vulkan fixture and extract the production barrier masks."""
from pathlib import Path
import json
import hashlib
import re

root = Path(__file__).resolve().parents[2]
out = root / 'build/immortals-menu-diagnostic-20261009/compute-indirect-tests'
out.mkdir(parents=True, exist_ok=True)

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

fixture = (root / 'tests/pr396/vulkan-synthetic.cpp').read_text()
fixture = fixture[fixture.index('struct HostBuffer'):fixture.index('int main(')]
for signature in ('    uint32_t ReorderCase(', '    void CopyBenchmark(',
                  '    uint32_t CopybackThenCpuWrite('):
    fixture = fixture.replace(function(fixture, signature), '')
fixture = fixture.replace('VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|',
                          'VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|')
fixture = fixture.replace('std::array<VkDescriptorSetLayoutBinding,2>', 'std::array<VkDescriptorSetLayoutBinding,3>')
fixture = fixture.replace('{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}}',
                          '{1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},'
                          '{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}}')
fixture = fixture.replace('lci.bindingCount=2;', 'lci.bindingCount=3;')
fixture = fixture.replace('plci.setLayoutCount=1;',
                          'VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,20};'
                          'plci.pushConstantRangeCount=1;plci.pPushConstantRanges=&push;plci.setLayoutCount=1;')
fixture = fixture.replace('VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}',
                          'VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6}')
fixture = fixture.replace('dpci.maxSets=1;', 'dpci.maxSets=2;')
fixture = fixture.replace('std::array<VkDescriptorBufferInfo,2> infos{{{buffers[1].buffer,0,4},{buffers[2].buffer,0,4}}}',
                          'std::array<VkDescriptorBufferInfo,3> infos{{{buffers[0].buffer,0,16},'
                          '{buffers[1].buffer,0,16},{buffers[2].buffer,0,16}}}')
fixture = fixture.replace('std::array<VkWriteDescriptorSet,2>', 'std::array<VkWriteDescriptorSet,3>')
fixture = fixture.replace('i<2;', 'i<3;')
fixture = fixture.replace('vkUpdateDescriptorSets(device,2,', 'vkUpdateDescriptorSets(device,3,')
(out / 'indirect-fixture.inc').write_text(fixture)

production = (root / 'src/yuzu_video_core/renderer_vulkan/vk_compute_pass.cpp').read_text()
assembly = function(production, 'std::pair<VkBuffer, VkDeviceSize> ComputeIndirectPass::Assemble(')
barriers = []
for name in ('input', 'output'):
    start = assembly.index(f'const VkMemoryBarrier {name}')
    end = assembly.index('};', start) + 2
    barriers.append(assembly[start:end])
(out / 'indirect-barriers.inc').write_text('\n'.join(barriers))
assert 'VK_PIPELINE_STAGE_ALL_COMMANDS_BIT' in assembly
assert 'VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT' in assembly
rasterizer = (root / 'src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
dispatch = function(rasterizer, 'void RasterizerVulkan::DispatchCompute(')
assert dispatch.index('compute_indirect_pass->Assemble(') < dispatch.index('pipeline->Configure(')
assert 'ObtainBuffer(*addresses[i],sizeof(u32)' in re.sub(r'\s+', '', dispatch)
assert 'qmd.grid_dim_y | (qmd.grid_dim_z << 16)' in dispatch
staging = (root / 'src/yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.cpp').read_text()
assert 'VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT' in staging
gl = (root / 'src/yuzu_video_core/renderer_opengl/gl_rasterizer.cpp').read_text()
gl_dispatch = function(gl, 'void RasterizerOpenGL::DispatchCompute(')
assert gl_dispatch.index('AssembleComputeIndirect(') < gl_dispatch.index('pipeline->Configure(')
assert 'ObtainBuffer(*addresses[i],sizeof(u32)' in re.sub(r'\s+', '', gl_dispatch)
assert 'glDispatchComputeIndirect(0)' in gl_dispatch
(out / 'extraction-manifest.json').write_text(json.dumps({
    'assembly_sha256': hashlib.sha256(assembly.encode()).hexdigest(),
    'dispatch_sha256': hashlib.sha256(dispatch.encode()).hexdigest()}, indent=2))
print('PASS production barrier masks, QMD packing, pipeline restoration and indirect-buffer usage')
