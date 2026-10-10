"""Extract production caller, depth bias update and topology tracker."""
from pathlib import Path
import subprocess,hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/depth-bias-dynamic-state-tests'
out.mkdir(parents=True,exist_ok=True)
def function(text,signature):
    begin=text.index(signature);p=text.index('{',begin);depth=1;end=p+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[begin:end]
path='src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp';manifest={}
for variant in ('baseline','fixed'):
    text=(root/path).read_text() if variant=='fixed' else subprocess.check_output(['git','show',f'307ccf09:{path}'],cwd=root,text=True)
    bodies=[function(text,'void RasterizerVulkan::'+name+'(') for name in ('UpdateDynamicStates','UpdateDepthBiasEnable')]
    body='\n\n'.join(bodies)
    (out/f'dynamic-state-{variant}.inc').write_text(body.replace('RasterizerVulkan::','Rasterizer::'))
    manifest[variant]=hashlib.sha256(body.encode()).hexdigest()
tracker=function((root/'src/yuzu_video_core/renderer_vulkan/vk_state_tracker.h').read_text(),'bool ChangePrimitiveTopology(')
(out/'dynamic-state-tracker.inc').write_text(tracker.replace('bool ChangePrimitiveTopology(','bool StateTracker::ChangePrimitiveTopology('))
(out/'extraction-manifest.json').write_text(json.dumps(manifest,indent=2))
print('Extracted production dynamic-state caller, bias enable and tracker.')
