from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parents[2]
out=root/'build/performance-live-20261011/retirement-audit-tests';out.mkdir(parents=True,exist_ok=True)
def body(path,signature):
    text=(root/path).read_text();a=text.index(signature);b=text.index('{',a)+1;depth=1
    while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
    return text[a:b]
fence=body('src/yuzu_video_core/fence_manager.h','void PopAsyncFlushes()')
query=body('src/yuzu_video_core/query_cache/query_cache.h','bool QueryCacheBase<Traits>::SemiFlushQueryDirty(')
(out/'retirement.inc').write_text(fence.replace('void PopAsyncFlushes()','void RetireModel::PopAsyncFlushes()'))
(out/'query.inc').write_text(query.replace('QueryCacheBase<Traits>::','QueryModel::'))
(out/'extraction.json').write_text(json.dumps({'retirement':hashlib.sha256(fence.encode()).hexdigest(),'raw_query_write':hashlib.sha256(query.encode()).hexdigest()},indent=2))
print('Extracted actual cache retirement locking and query raw RAM write.')
