from pathlib import Path
import hashlib, json, subprocess
root=Path(__file__).resolve().parents[2]
out=root/'build/performance-live-20261011/download-tests'
out.mkdir(parents=True,exist_ok=True)
manifest={}
for mode in ('current','baseline'):
    path='src/yuzu_video_core/buffer_cache/buffer_cache.h'
    source=(root/path).read_text() if mode=='current' else subprocess.check_output(['git','show',f'61f2d4b6:{path}'],cwd=root,text=True)
    bodies=[]
    names=['CommitAsyncFlushesHigh'] if mode=='baseline' else ['AccumulateFlushes','CommitAsyncFlushesHigh','PopAsyncBuffers','InvalidateReadbackReferences','ClearDownload']
    for name in names:
        start=source.index(f'void BufferCache<P>::{name}(')
        opening=source.index('{',start);end=opening+1;depth=1
        while depth: depth+=(source[end]=='{')-(source[end]=='}');end+=1
        body=source[start:end]
        manifest[f'{mode}/{name}']=hashlib.sha256(body.encode()).hexdigest()
        body=body.replace('BufferCache<P>::','DownloadHarness::')
        if mode=='baseline':
            body=body.replace('::CommitAsyncFlushesHigh(', '::CommitBaseline(')
            body=body.replace('pending_downloads.emplace_back(std::move(normalized_copies));',
                'pending_readback_flags.emplace_back(std::nullopt);\n'
                'pending_readback_flag_offsets.emplace_back(normalized_copies.size(), std::numeric_limits<u64>::max());\n'
                'pending_readback_cpu_copies.emplace_back(normalized_copies.size());\n'
                'pending_downloads.emplace_back(std::move(normalized_copies));')
        bodies.append(body)
    (out/f'{mode}.inc').write_text('\n'.join(bodies))
(out/'extraction.json').write_text(json.dumps(manifest,indent=2))
print('Extracted exact production download planning and retirement bodies.')
