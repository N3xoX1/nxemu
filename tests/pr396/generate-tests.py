"""Extract production function bodies unchanged; only replace their enclosing class name.
Memory, the GPU runtime and channel setup are small deterministic test doubles.
RangeSet, OverlapRangeSet, small_vector and DescriptorPayload are production headers.
"""
from pathlib import Path
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/pr396-corrections/tests'
OUT.mkdir(parents=True, exist_ok=True)

def source(mode, relative):
    if mode == 'fixed':
        return (ROOT / 'src' / relative).read_text()
    commit = {'master': 'c32270c39249ada403c518c4bb28e8f807f00e1a',
              'pr': 'fa29fce321314eb99b49f083fd5601818450a6e3'}[mode]
    return subprocess.check_output(['git', 'show', f'{commit}:src/{relative}'], cwd=ROOT, text=True)

def functions(source, signature):
    matches = []
    pos = 0
    while (start := source.find(signature, pos)) != -1:
        opening = source.index('{', start)
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        # Function bodies in this selection have no unmatched braces in comments/strings.
        matches.append(source[start:end])
        pos = end
    return matches

manifest = {}
for mode in ('master', 'pr', 'fixed'):
    cache = source(mode, 'yuzu_video_core/buffer_cache/buffer_cache.h')
    bodies = []
    for signature in ['bool BufferCache<P>::SynchronizeBuffer(',
                      'void BufferCache<P>::ClearDownload(',
                      'void BufferCache<P>::PopAsyncBuffers(',
                      'typename BufferCache<P>::SparseBinding BufferCache<P>::SynchronizeSparseBuffer(']:
        bodies.extend(functions(cache, signature))
    if mode != 'master':
        for signature in ['GPUVAddr BufferCache<P>::FindSparseAlias(',
                          'void BufferCache<P>::MarkSparseWrite(',
                          'void BufferCache<P>::CopySparseWrites(']:
            bodies.extend(functions(cache, signature))
    rewritten = '\n\n'.join(bodies).replace('BufferCache<P>::', 'Harness::').replace('typename Harness::', 'Harness::')
    (OUT / f'{mode}-functions.inc').write_text(rewritten)
    manifest[mode] = [hashlib.sha256(b.encode()).hexdigest() for b in bodies]
    dma = source(mode, 'yuzu_video_core/dma_pusher.cpp')
    start = dma.index('        const bool ', dma.index('if (header.size > 0)'))
    end = dma.index('        if (use_safe)', start)
    (OUT / f'{mode}-dma.inc').write_text(dma[start:end])

scheduler = source('fixed', 'yuzu_video_core/renderer_vulkan/vk_scheduler.cpp')
tracking = functions(scheduler, 'void Scheduler::MarkBufferWrite(') + functions(scheduler, 'bool Scheduler::IsBufferWritten(')
(OUT / 'tracking.inc').write_text('\n\n'.join(tracking).replace('Scheduler::', 'HazardTracker::'))
dma = source('fixed', 'yuzu_video_core/dma_pusher.cpp')
parser = functions(dma, 'void DmaPusher::ProcessCommands(') + functions(dma, 'void DmaPusher::SetState(')
(OUT / 'parser.inc').write_text('\n\n'.join(parser).replace('DmaPusher::', 'Parser::'))
header = source('fixed', 'yuzu_video_core/dma_pusher.h')
mode = re.search(r'enum class SubmissionMode.*?\n};', header, re.S).group()
command = re.search(r'union CommandHeader.*?\n};', header, re.S).group()
(OUT / 'command-types.inc').write_text(mode + '\n' + command)
runtime = source('fixed', 'yuzu_video_core/renderer_vulkan/vk_buffer_cache.cpp')
version = functions(source('fixed', 'yuzu_video_core/buffer_cache/buffer_cache_base.h'),
                    'void MarkBufferContentChanged(')[0]
(OUT / 'content-version.inc').write_text(version)
upload = functions(runtime, 'bool BufferCacheRuntime::CanReorderUpload(')[0]
(OUT / 'reorder-upload-body.inc').write_text(upload[upload.index('{'):])
import json
(OUT / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2))
query = source('fixed', 'yuzu_video_core/renderer_vulkan/vk_query_cache.cpp')
query_sync = functions(query, 'void QueryCacheRuntime::SyncValues(')[0]
assert 'impl->scheduler.MarkBufferWrite(buffer);' in query_sync
assert 'buffer->MarkUsage(' in query_sync
(OUT / 'query-integration.inc').write_text('#define FIXED_QUERY_MARK 1\n')
manifest['query_sync'] = hashlib.sha256(query_sync.encode()).hexdigest()
(OUT / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2))
print('Extracted exact master, PR and corrected functions; checked query write registration.')
