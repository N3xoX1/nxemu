"""Use actual production barrier masks in the Vulkan regression fixture."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/capture-analysis'
raster=(root/'src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
query=(root/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp').read_text()
texture=(root/'src/yuzu_video_core/renderer_vulkan/vk_texture_cache.cpp').read_text()
scheduler=(root/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp').read_text()
compute=scheduler[scheduler.index('void Scheduler::ComputeMemoryBarrier('):scheduler.index('void Scheduler::EndRenderPass(')]
graphics=(root/'src/yuzu_video_core/renderer_vulkan/vk_graphics_pipeline.cpp').read_text()
assert 'cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);\n            Scheduler::ComputeMemoryBarrier(cmdbuf);' in raster
assert 'cmdbuf.Dispatch(dim[0], dim[1], dim[2]);\n        Scheduler::ComputeMemoryBarrier(cmdbuf);' in raster
assert 'cmdbuf.Dispatch(groups, 1, 1);\n            Scheduler::ComputeMemoryBarrier(cmdbuf);' in graphics
full_query=(root/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp').read_text()
sync=full_query[full_query.index('void QueryCacheRuntime::SyncValues('):]
assert sync.index('Barriers(true);') < sync.index('cmdbuf.CopyBuffer(') < sync.index('Barriers(false);')
wfi=raster[raster.index('void RasterizerVulkan::WaitForIdle()'):raster.index('void RasterizerVulkan::FragmentBarrier()')]
query=query[query.index('void QueryCacheRuntime::Barriers('):query.index('template <typename SyncValuesType>')]
texture=texture[texture.index('void Image::DownloadMemory(std::span<VkBuffer>'):]
def mask(text, field):
    return re.search(r'\.'+field+r'\s*=\s*([^,]+),',text)[1].strip()
flags=re.search(r'VkPipelineStageFlags flags\s*=([^;]+);',wfi)[1].strip()
pre=query[query.index('READ_BARRIER'):query.index('WRITE_BARRIER')]
post=query[query.index('WRITE_BARRIER'):]
download=texture[texture.index('const VkImageMemoryBarrier image_write_barrier'):]
values={'wfi_stages':flags,'wfi_src':mask(wfi,'srcAccessMask'),
        'wfi_dst':mask(wfi,'dstAccessMask'),'query_pre_src':mask(pre,'srcAccessMask'),
        'query_pre_dst':mask(pre,'dstAccessMask'),'query_post_src':mask(post,'srcAccessMask'),
        'query_post_dst':mask(post,'dstAccessMask'),'download_dst':mask(download,'dstAccessMask'),
        'compute_src':mask(compute,'srcAccessMask'),'compute_dst':mask(compute,'dstAccessMask')}
(out/'barrier-values.inc').write_text('\n'.join('constexpr auto '+k+' = '+v+';' for k,v in values.items())+'\n')
print('PASS extracted actual WFI, query copy and image restore masks')
