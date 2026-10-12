from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'build/performance-live-20261011/coalesced-tests'
out.mkdir(parents=True,exist_ok=True)
source=(root/'src/yuzu_video_core/host1x/gpu_device_memory_manager.cpp').read_text()
bodies=[];manifest={}
for name in ('WalkBlock','WriteBlockForGpuReadback','WriteBlockUnsafe'):
    start=source.index(f'void DeviceMemoryManager<Traits>::{name}(')
    opening=source.index('{',start);end=opening+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    body=source[start:end];manifest[name]=hashlib.sha256(body.encode()).hexdigest()
    body=body.replace('DeviceMemoryManager<Traits>::','CopyHarness::')
    bodies.append(body)
(out/'device-copy.inc').write_text('\n'.join(bodies))
(out/'extraction.json').write_text(json.dumps(manifest,indent=2))
print('Extracted complete production device walker and both copy methods.')
