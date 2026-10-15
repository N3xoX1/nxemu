"""Exercise production binding resolution on ordinary disjoint GPU mappings."""
from pathlib import Path
import subprocess, hashlib, json
root=Path(__file__).resolve().parents[2]
out=root/'build/monster-hunter-rise-20261013/storage-tests'
out.mkdir(parents=True,exist_ok=True)
def function(text,signature):
    start=text.index(signature); opening=text.index('{',start); depth=1; end=opening+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
path='src/yuzu_video_core/buffer_cache/buffer_cache.h'
baseline='1f5f552cb71527a0f42969bc963843576bc4f9ef'
hashes={}
for variant in ('baseline','fixed'):
    text=(root/path).read_text() if variant=='fixed' else subprocess.check_output(['git','show',f'{baseline}:{path}'],cwd=root,text=True)
    body=function(text,'Binding BufferCache<P>::StorageBufferBinding(')
    (out/f'{variant}-storage.inc').write_text(body.replace('BufferCache<P>::','Harness::'))
    hashes[variant]=hashlib.sha256(body.encode()).hexdigest()
types=(root/'src/yuzu_video_core/buffer_cache/buffer_cache_base.h').read_text()
(out/'binding.inc').write_text(function(types,'struct Binding {')+';')
harness=(root/'tests/immortals-gpu/sparse-storage-binding.cpp').read_text()
harness=harness.replace('#include <iostream>','#include <iostream>\n#include <limits>')
harness=harness.replace('bool prefix_mapped=false,sparse=true,later_pages_mapped=true;', 'bool prefix_mapped=false,sparse=true,later_pages_mapped=true;\n    std::vector<std::pair<GPUVAddr,std::size_t>> ordinary_ranges;')
harness=harness.replace('GetSubmappedRange(GPUVAddr addr,u32 span)const{', 'GetSubmappedRange(GPUVAddr addr,u64 span)const{\n        if(!ordinary_ranges.empty())return ordinary_ranges;')
cases='''
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4096}};auto b=h.Resolve();Check("ordinary_disjoint_pages_read_full_descriptor",b.is_sparse&&b.size==8192&&b.sparse_gpu_addr==0x10000);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4096}};auto b=h.Resolve(true);Check("ordinary_disjoint_pages_write_full_descriptor",b.is_sparse&&b.size==8192);}
    {Harness h;h.memory.base+=32;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4064;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4128}};auto b=h.Resolve(true);Check("ordinary_unaligned_disjoint_descriptor_keeps_offset",b.is_sparse&&b.size==8224&&b.sparse_gpu_addr==0x10000);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.size=16384;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4096},{0x13000,4096}};auto b=h.Resolve(true);Check("ordinary_disjoint_prefix_stops_at_guest_hole",b.is_sparse&&b.size==8192);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.size=12288;h.memory.ordinary_ranges={{0x10000,4096},{0x12000,4096}};auto b=h.Resolve(true);Check("ordinary_hole_does_not_expose_later_allocation",!b.is_sparse&&b.size==4096);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.size=0;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4096}};auto b=h.Resolve(false,3);Check("ordinary_custom_cbuf_assembles_layout",b.is_sparse&&b.size==8192);}
    {Harness h;h.memory.prefix_mapped=true;h.memory.sparse=false;h.memory.continuous=4096;h.memory.size=0;h.memory.ordinary_ranges={{0x10000,4096},{0x11000,4096}};auto b=h.Resolve(true,3);Check("ordinary_custom_cbuf_write_keeps_linear_prefix",!b.is_sparse&&b.size==4096);}
'''
harness=harness.replace('    const auto begin=',cases+'    const auto begin=')
(out/'storage-binding.cpp').write_text(harness)
(out/'extraction.json').write_text(json.dumps(dict(baseline_commit=baseline,sha256=hashes),indent=2))
