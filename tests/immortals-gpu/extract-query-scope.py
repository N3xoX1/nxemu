from pathlib import Path
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/capture-analysis'
query=(root/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp').read_text()
query=query[query.index('class SamplesStreamer'):query.index('class PrimitivesSucceededStreamer')]
counter=query[query.index('void StartCounter()'):query.index('void ResetCounter()')]
assert 'RequestOutsideRenderPassOperationContext()' not in counter
(out/'query-counter.inc').write_text(counter.replace(' override', ''))
scheduler=(root/'src/yuzu_video_core/renderer_vulkan/vk_scheduler.cpp').read_text()
begin=scheduler[scheduler.index('void Scheduler::RequestRenderpass('):scheduler.index('void Scheduler::RequestOutsideRenderPassOperationContext(')]
end=scheduler[scheduler.index('void Scheduler::EndRenderPass()'):scheduler.index('void Scheduler::AcquireNewChunk()')]
assert begin.index('cmdbuf.BeginRenderPass(') < begin.index('NotifyRenderPass(true)')
assert end.index('NotifyRenderPass(false)') < end.index('cmdbuf.EndRenderPass(')
raster=(root/'src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
draw=raster[raster.index('void RasterizerVulkan::PrepareDraw('):raster.index('void RasterizerVulkan::DrawVtgAsCompute(')]
assert draw.index('query_cache.CounterEnable(')<draw.index('pipeline->Configure(')<draw.index('draw_func(')
util=(root/'src/yuzu_video_core/renderer_vulkan/present/util.cpp').read_text()
transition=util[util.index('void TransitionImageLayout('):util.index('void UploadImage(')]
(out/'presentation-transition.inc').write_text(transition)
print('PASS production query callbacks and pass ordering; exact query methods and presentation transition extracted')
