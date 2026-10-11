"""Extract the production DMA eligibility checks and fallback before compiling the fixture."""
from pathlib import Path
import subprocess,hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'build/immortals-menu-diagnostic-20261009/dma-copy-tests'
out.mkdir(parents=True,exist_ok=True)
path='src/yuzu_video_core/buffer_cache/buffer_cache.h'
manifest={}
for variant in ['baseline','fixed']:
    source=(root/path).read_text() if variant=='fixed' else subprocess.check_output(['git','show',f'b71029e5:{path}'],cwd=root,text=True)
    start=source.index('bool BufferCache<P>::DMACopy(')
    end=source.index('    const bool source_dirty',start)
    checks=source[start:end].replace('BufferCache<P>::DMACopy','Harness::CanAccelerate')+'    return true;\n}\n'
    (out/f'{variant}-checks.inc').write_text(checks)
    manifest[variant]=hashlib.sha256(source[start:end].encode()).hexdigest()
source=(root/'src/yuzu_video_core/engines/maxwell_dma.cpp').read_text()
start=source.index('                    // Snapshot the source before writing.')
end=source.index('\n                }',start)
fallback=source[start:end]
(out/'fixed-fallback.inc').write_text(fallback)
manifest['fallback']=hashlib.sha256(fallback.encode()).hexdigest()
(out/'extraction-manifest.json').write_text(json.dumps(manifest,indent=2))
print('PASS extracted actual DMA checks and snapshot fallback')
