"""Extract the storage binding resolver unchanged, with real Binding fields."""
from pathlib import Path
import subprocess,json,hashlib
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/sparse-storage-tests'
out.mkdir(parents=True,exist_ok=True)
def function(text,signature):
    start=text.index(signature);opening=text.index('{',start);depth=1;end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
path='src/yuzu_video_core/buffer_cache/buffer_cache.h';hashes={}
for variant in ('baseline','fixed'):
    text=(root/path).read_text() if variant=='fixed' else subprocess.check_output(['git','show',f'ca47b3b2:{path}'],cwd=root,text=True)
    body=function(text,'Binding BufferCache<P>::StorageBufferBinding(')
    (out/f'{variant}-storage.inc').write_text(body.replace('BufferCache<P>::','Harness::'))
    hashes[variant]=hashlib.sha256(body.encode()).hexdigest()
types=(root/'src/yuzu_video_core/buffer_cache/buffer_cache_base.h').read_text()
(out/'binding.inc').write_text(function(types,'struct Binding {')+';')
(out/'extraction-manifest.json').write_text(json.dumps(hashes,indent=2))
print('Extracted baseline/current storage binding resolver and production Binding.')
