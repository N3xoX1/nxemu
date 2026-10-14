from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[3]
out=root/'build/performance-review-worker-batching'
out.mkdir(parents=True,exist_ok=True)
path='src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp'
def function(text,name):
    start=text.index(name);opening=text.index('{',start);end=opening+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
baseline=subprocess.check_output(['git','show','e7dde4e9:'+path],cwd=root,text=True)
current=(root/path).read_text()
for name,text in [('baseline',baseline),('candidate',current)]:
    body=function(text,'void RasterizerVulkan::FlushWork(')
    (out/(name+'.inc')).write_text('namespace '+name+' { struct RasterizerVulkan { u32 draw_counter{}; Scheduler scheduler; void FlushWork(); };\n'+body+'\n}\n')
print('Extracted exact production FlushWork and pre-batching reference')
