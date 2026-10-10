"""Extract the production depth bias update and topology tracker for a deterministic regression."""
from pathlib import Path
import subprocess,json,hashlib
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/depth-bias-topology-tests'
out.mkdir(parents=True,exist_ok=True)
def function(text,signature):
    start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
hashes={}
for variant in ('baseline','fixed'):
    path='src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp'
    text=(root/path).read_text() if variant=='fixed' else subprocess.check_output(['git','show',f'4d053d0d:{path}'],cwd=root,text=True)
    body=function(text,'void RasterizerVulkan::UpdateDepthBiasEnable(')
    (out/f'depth-bias-{variant}.inc').write_text(body.replace('RasterizerVulkan::','Rasterizer::'))
    hashes[variant]=hashlib.sha256(body.encode()).hexdigest()
tracker=function((root/'src/yuzu_video_core/renderer_vulkan/vk_state_tracker.h').read_text(),'bool ChangePrimitiveTopology(')
(out/'depth-bias-tracker.inc').write_text(tracker.replace('bool ChangePrimitiveTopology(','bool StateTracker::ChangePrimitiveTopology('))
(out/'extraction-manifest.json').write_text(json.dumps(hashes,indent=2))
print('Extracted production depth bias update and topology tracker.')
