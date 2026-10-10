from pathlib import Path
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/capture-analysis'
query=(root/'src/yuzu_video_core/renderer_vulkan/vk_query_cache.cpp').read_text()
query=query[query.index('class SamplesStreamer'):query.index('class PrimitivesSucceededStreamer')]
start=query[query.index('void StartCounter()'):query.index('void PauseCounter()')]
pause=query[query.index('void PauseCounter()'):query.index('void ResetCounter()')]
assert start.index('RequestOutsideRenderPassOperationContext()') < start.index('cmdbuf.BeginQuery(')
assert pause.index('RequestOutsideRenderPassOperationContext()') < pause.index('cmdbuf.EndQuery(')
raster=(root/'src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp').read_text()
draw=raster[raster.index('void RasterizerVulkan::PrepareDraw('):raster.index('void RasterizerVulkan::DrawVtgAsCompute(')]
assert draw.index('query_cache.CounterEnable(')<draw.index('pipeline->Configure(')<draw.index('draw_func(')
util=(root/'src/yuzu_video_core/renderer_vulkan/present/util.cpp').read_text()
transition=util[util.index('void TransitionImageLayout('):util.index('void UploadImage(')]
(out/'presentation-transition.inc').write_text(transition)
print('PASS production query scope and draw ordering; exact presentation transition extracted')
