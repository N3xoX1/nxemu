"""Exercise unchanged production DMA/compute methods with deterministic memory doubles."""
from pathlib import Path
import subprocess
import hashlib
import json

root = Path(__file__).resolve().parents[2]
out = root / 'build/immortals-menu-diagnostic-20261009/indirect-dispatch-tests'
out.mkdir(parents=True, exist_ok=True)

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

hashes = {}
for variant in ('baseline', 'fixed'):
    def read(path):
        if variant == 'fixed':
            return (root / path).read_text()
        return subprocess.check_output(['git', 'show', f'25a0b0e4:{path}'], cwd=root, text=True)
    compute = read('src/yuzu_video_core/engines/kepler_compute.cpp')
    methods = '\n\n'.join(function(compute, s) for s in
        ('void KeplerCompute::CallMethod(', 'void KeplerCompute::CallMultiMethod('))
    (out / f'{variant}-compute.inc').write_text(methods.replace('KeplerCompute::', 'Compute::'))
    dma = read('src/yuzu_video_core/dma_pusher.cpp')
    start = dma.index('    if (header.size > 0 &&')
    end = dma.index('    if (header.size > 0)\n', start)
    block = dma[start:end]
    (out / f'{variant}-dma-origin.inc').write_text(block)
    hashes[variant] = {'compute': hashlib.sha256(methods.encode()).hexdigest(),
                       'dma_origin': hashlib.sha256(block.encode()).hexdigest()}
(out / 'extraction-manifest.json').write_text(json.dumps(hashes, indent=2))
print('Extracted baseline and fixed production DMA/compute methods.')
