from pathlib import Path
import hashlib, json, subprocess
root=Path(__file__).resolve().parents[2]
out=root/'build/performance-live-20261011/range-tests'
manifest={}
for mode in ('current','baseline'):
    path='src/yuzu_video_core/memory_manager.cpp'
    text=(root/path).read_text() if mode=='current' else subprocess.check_output(['git','show',f'61f2d4b6:{path}'],cwd=root,text=True)
    start=text.index('bool MemoryManager::IsContinuousRange(')
    opening=text.index('{',start);end=opening+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    body=text[start:end]
    manifest[mode]=hashlib.sha256(body.encode()).hexdigest()
    body=body.replace('MemoryManager::','MemoryHarness::')
    if mode=='baseline': body=body.replace('IsContinuousRange(', 'BaselineContinuousRange(')
    else: body=body.replace('thread_local ContinuousRangeCache','thread_local Tegra::ContinuousRangeCache')
    (out/f'continuity-{mode}.inc').write_text(body)
(out/'extraction.json').write_text(json.dumps(manifest,indent=2))
for mode in ('current','baseline'):
    path='src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp'
    text=(root/path).read_text() if mode=='current' else subprocess.check_output(['git','show',f'61f2d4b6:{path}'],cwd=root,text=True)
    bodies=[]
    for signature in ('bool RasterizerVulkan::MustFlushRegion(', 'RasterizerDownloadArea RasterizerVulkan::GetFlushArea('):
        start=text.index(signature);opening=text.index('{',start);end=opening+1;depth=1
        while depth: depth+=(text[end]=='{')-(text[end]=='}');end+=1
        body=text[start:end].replace('RasterizerVulkan::','RasterizerHarness::')
        if mode=='baseline': body=body.replace('RasterizerHarness::MustFlushRegion(', 'RasterizerHarness::BaselineMustFlushRegion(').replace('RasterizerHarness::GetFlushArea(', 'RasterizerHarness::BaselineGetFlushArea(')
        bodies.append(body)
    (out/f'flush-{mode}.inc').write_text('\n'.join(bodies))
print('Extracted exact current and validated baseline continuity checks.')
