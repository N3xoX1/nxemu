from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'build/performance-live-20261011/cache-filter-tests'
out.mkdir(parents=True,exist_ok=True)
def body(path,signature):
    text=(root/path).read_text();a=text.index(signature);b=text.index('{',a)+1;depth=1
    while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
    return text[a:b]
methods=[];manifest={}
for name in ('MustFlushRegion','OnCPUWrite'):
    b=body('src/yuzu_video_core/renderer_vulkan/vk_rasterizer.cpp',f'bool RasterizerVulkan::{name}(')
    manifest[name]=hashlib.sha256(b.encode()).hexdigest();methods.append(b.replace('RasterizerVulkan::','RasterizerModel::'))
b=body('src/yuzu_video_core/buffer_cache/buffer_cache_base.h','bool MayHaveGpuWrites(')
manifest['MayHaveGpuWrites']=hashlib.sha256(b.encode()).hexdigest()
methods.append(b.replace('bool MayHaveGpuWrites(','bool BufferModel::MayHaveGpuWrites('))
b=body('src/yuzu_video_core/buffer_cache/buffer_cache.h','void BufferCache<P>::MarkWrittenBuffer(')
manifest['MarkWrittenBuffer']=hashlib.sha256(b.encode()).hexdigest()
methods.append(b.replace('BufferCache<P>::','WrittenModel::'))
# Check every byte-dirty insertion site is covered, including DMA mirroring and
# sparse part publication. The sparse body also executes in the PR396 fixture.
text=(root/'src/yuzu_video_core/buffer_cache/buffer_cache.h').read_text()
adds=[line.strip() for line in text.splitlines() if 'gpu_modified_ranges.Add(' in line and 'uncommitted_' not in line]
assert len(adds)==3,adds
dma=body('src/yuzu_video_core/buffer_cache/buffer_cache.h','bool BufferCache<P>::DMACopy(')
assert dma.index('gpu_write_history.Mark(new_base_address, size)') < dma.index('tmp_intervals.push_back(')
sparse=body('src/yuzu_video_core/buffer_cache/buffer_cache.h','void BufferCache<P>::MarkSparseWrite(')
assert sparse.index('gpu_write_history.Mark(device_addr, write_size)') < sparse.index('memory_tracker.MarkRegionAsGpuModified(')
manifest['DMACopy']=hashlib.sha256(dma.encode()).hexdigest();manifest['MarkSparseWrite']=hashlib.sha256(sparse.encode()).hexdigest()
(out/'rasterizer.inc').write_text('\n\n'.join(methods))
(out/'extraction.json').write_text(json.dumps(manifest,indent=2))
print('Extracted actual Vulkan CPU-write/flush predicates and ordinary GPU write publication; checked all three dirty insertion sites.')
