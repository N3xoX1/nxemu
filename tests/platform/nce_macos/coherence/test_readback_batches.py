#!/usr/bin/env python3
"""Execute production DownloadMemory with a deferred GPU and staging lifetime checks.

No Metal/FPS forecast: measure fence counts and check every published byte against
an independent sequential oracle. Run from any cwd; clang++ must be available.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[4]
source = (ROOT / 'src/yuzu_video_core/texture_cache/texture_cache.h').read_text()
start = source.index('template <class P>\nvoid TextureCache<P>::DownloadMemory(')
end = source.index('\ntemplate <class P>', start + 10)
method = source[start:end]
prefix = r'''
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <memory>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
using u8=uint8_t;using u64=uint64_t;using DAddr=u64;using ImageId=size_t;
constexpr size_t operator""_MiB(unsigned long long n){return n*1024*1024;}
namespace boost::container {template<class T,size_t>using small_vector=std::vector<T>;}
enum class ImageFlagBits:unsigned {GpuModified=1,CpuModified=2};
ImageFlagBits operator~(ImageFlagBits x){return ImageFlagBits(~unsigned(x));}
ImageFlagBits operator&(ImageFlagBits x,ImageFlagBits y){return ImageFlagBits(unsigned(x)&unsigned(y));}
ImageFlagBits& operator&=(ImageFlagBits&x,ImageFlagBits y){return x=x&y;}
void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(99);}}
struct Info {size_t dest;};
struct Map {std::shared_ptr<std::array<u8,8>>storage;std::span<u8>mapped_span;unsigned id;bool reserved;};
struct Runtime {
 struct Job{Map map;u8 value;};std::vector<Job> jobs;
 unsigned waits=0,allocs=0,frees=0;size_t live=0,peak=0;std::array<bool,2048>ready{};
 Map DownloadStagingBuffer(size_t n,bool deferred=false){
  require(allocs<ready.size(),"bounded test allocations");auto s=std::make_shared<std::array<u8,8>>();
  live+=n;peak=std::max(live,peak);return {s,*s,allocs++,deferred};
 }
 void Finish(){++waits;for(auto&j:jobs){j.map.mapped_span[0]=j.value;ready[j.map.id]=true;}jobs.clear();}
 void FreeDeferredStagingBuffer(Map&m){require(m.reserved&&ready[m.id],"reserved staging released after completion");++frees;}
};
struct Image {ImageFlagBits flags=ImageFlagBits(1);u64 modification_tick=1;size_t unswizzled_size_bytes=8;u64 gpu_addr=0;Info info{};u8 value=0;Runtime*rt=nullptr;
 bool IsSafeDownload()const{return unsigned(flags&ImageFlagBits::GpuModified)&&!unsigned(flags&ImageFlagBits::CpuModified);}
 void DownloadMemory(Map&map,int){rt->jobs.push_back({map,value});}
};using ImageBase=Image;
struct Memory{Runtime*rt;std::array<u8,32> bytes{};std::vector<std::pair<size_t,u8>>published;};
int FullDownloadCopies(Info){return 0;}
void SwizzleImage(Memory&mem,u64,Info info,int,std::span<u8>d,int&){require(mem.rt->jobs.empty(),"all copies complete before CPU publication");mem.bytes[info.dest]=d[0];mem.published.emplace_back(info.dest,d[0]);}
template<class P>struct TextureCache {
 using AsyncBuffer=Map;std::vector<Image>slot_images;Runtime runtime;Memory memory{&runtime};Memory*gpu_memory=&memory;int swizzle_data_buffer{};unsigned updates=0;
 template<class F>void ForEachImageInRegion(DAddr,size_t,F f){for(size_t i=0;i<slot_images.size();++i)f(i,slot_images[i]);}
 void UpdateNativeReadTracking(Image&im,ImageId){++updates;require(runtime.jobs.empty(),"tracking updated after completion");}
 void MarkReadbackSynchronized(Image&){}
 void DownloadMemory(DAddr,size_t,bool);
};
'''
main = r'''
int main(){
 bool release=std::getenv("NXEMU_NCE_RELEASE_READBACK")!=nullptr;
 bool batch=std::getenv("NXEMU_NCE_BATCH_READBACK")!=nullptr;unsigned cases=0;u64 total_waits=0;
 for(unsigned seed=0;seed<160;++seed)for(bool native:{false,true}){
  TextureCache<int>t;std::mt19937 rng(seed);size_t count=seed<8?std::array<size_t,8>{0,1,2,16,63,64,65,193}[seed]:rng()%256;
  std::vector<Image>expected;
  for(size_t i=0;i<count;++i){Image im;im.rt=&t.runtime;im.modification_tick=rng()%16;im.info.dest=rng()%32;im.value=rng();
   // Includes oversized single images, exact boundaries and tiny packed textures.
   im.unswizzled_size_bytes=seed<8?8:std::array<size_t,5>{8,512,4_MiB,32_MiB,65_MiB}[rng()%5];
   im.flags=ImageFlagBits(rng()%5==0?3:1);t.slot_images.push_back(im);if(im.IsSafeDownload())expected.push_back(im);
  }
  std::ranges::sort(expected,[](auto&a,auto&b){return a.modification_tick<b.modification_tick;});
  std::array<u8,32>oracle{};std::vector<std::pair<size_t,u8>>publications;unsigned waits=0;size_t n=0,bytes=0;
  for(auto&im:expected){oracle[im.info.dest]=im.value;publications.emplace_back(im.info.dest,im.value);
   if(native&&batch){if(n&&(n>=64||bytes>=64_MiB||im.unswizzled_size_bytes>64_MiB-bytes)){++waits;n=bytes=0;}++n;bytes+=im.unswizzled_size_bytes;}
   else ++waits;
  }
  if(n)++waits;
  t.DownloadMemory(0,16384,native);
  require(t.memory.bytes==oracle&&t.memory.published==publications,"byte oracle and alias publication order");
  require(t.runtime.waits==waits,"expected bounded fence count");require(t.updates==expected.size(),"tracking updated once per published image");
  require(t.runtime.allocs==expected.size(),"one independent allocation per selected image");
  require(t.runtime.frees==(native&&batch?expected.size():0),"all reserved allocations freed");
  for(auto&im:t.slot_images)if(unsigned(im.flags&ImageFlagBits::CpuModified))require(unsigned(im.flags&ImageFlagBits::GpuModified),"partial CPU writes preserve ownership");
  total_waits+=waits;++cases;
  if(release||!native){
   auto before=t.runtime.allocs;t.DownloadMemory(0,16384,native);
   require(t.runtime.allocs==before,"completed readback is not copied again without a GPU write");
   if(!t.slot_images.empty()){
    auto&im=t.slot_images[0];im.flags=ImageFlagBits(1);++im.modification_tick;im.value=173;
    t.DownloadMemory(0,16384,native);
    require(t.runtime.allocs==before+1&&t.memory.bytes[im.info.dest]==173,"next GPU write is read back afresh");
   }
  }
 }
 printf("PASS %u production texture combinations; waits=%llu; batch=%d\n",cases,(unsigned long long)total_waits,batch);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-batch-') as directory:
    p=Path(directory);(p/'test.cpp').write_text(prefix+method+main)
    subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    for enabled, release in ((False,False),(True,False),(False,True),(True,True)):
        env=dict(os.environ);env.pop('NXEMU_NCE_BATCH_READBACK',None);env.pop('NXEMU_NCE_RELEASE_READBACK',None)
        if enabled:env['NXEMU_NCE_BATCH_READBACK']='1'
        if release:env['NXEMU_NCE_RELEASE_READBACK']='1'
        subprocess.run([str(p/'test')],env=env,check=True)
