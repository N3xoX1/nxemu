from pathlib import Path
p=Path(__file__).resolve().parent
repo=p.parents[2]
def function(text,name):
 start=text.index(name);opening=text.index('{',start);end=opening+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
src=(repo/'src/yuzu_video_core/renderer_vulkan/vk_graphics_pipeline.cpp').read_text()
body=function(src,'void GraphicsPipeline::ConfigureDraw(')
body=body[body.index('    if (!uses_texture_table) {'):body.index('    if (bind_pipeline && device.GetDriverID()')]
(p/'candidate-draw.inc').write_text(body)
src=(repo/'src/yuzu_video_core/engines/maxwell_3d.cpp').read_text()
body=function(src,'void Maxwell3D::ProcessDirtyRegisters(').replace('Maxwell3D::ProcessDirtyRegisters','Harness::ProcessCandidate')
prefix=(p/'dirty-reference-candidate.inc').read_text().split('void Harness::ProcessCandidate')[0]
(p/'dirty-selected.inc').write_text(prefix+body)
src=(repo/'src/yuzu_video_core/renderer_vulkan/vk_resource_pool.cpp').read_text()
(p/'pool-selected.inc').write_text(function(src,'size_t ResourcePool::CommitResource(').replace('ResourcePool::','Pool::'))
print('Extracted current production helpers')
