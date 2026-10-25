#!/usr/bin/env python3
"""Production async queue offsets and partial-publication routing, ASan/UBSan."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4];source=(root/'src/yuzu_video_core/texture_cache/texture_cache.h').read_text()
def block(start):
 a=source.index('{',start);i=a+1;depth=1
 while depth:
  depth+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[a:i]
a=source.index('void TextureCache<P>::CommitAsyncFlushes()');commit='template<class P> void TextureCache<P>::CommitAsyncFlushes()'+block(a)
a=source.index('void TextureCache<P>::PopAsyncFlushes()');b=source.index('    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS)',a)
pop='template<class P> void TextureCache<P>::PopAsyncFlushes(){'+source[source.index('{',a)+1:b]+block(b)+'}'
code=r'''
#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <cstdio>
#include <cstdlib>
using u8=unsigned char;using u64=unsigned long long;using ImageId=size_t;
void check(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(99);}}
#define ASSERT(x) check(x,"partial eligibility invariant")
namespace Common {template<class T>constexpr T AlignUp(T n,size_t a){return (n+a-1)/a*a;}}
namespace boost::container{template<class T,size_t>using small_vector=std::vector<T>;}
constexpr bool IMPLEMENTS_ASYNC_DOWNLOADS=true;
enum class ImageFlagBits{Remapped=1};bool False(ImageFlagBits f){return int(f)==0;}ImageFlagBits operator&(ImageFlagBits a,ImageFlagBits b){return ImageFlagBits(int(a)&int(b));}
struct Map{std::shared_ptr<std::vector<u8>>data;std::span<u8>mapped_span;size_t offset=0;};
struct Memory{std::map<u64,u8>output;std::optional<u64>GpuToCpuAddress(u64 a,size_t){return a+0x100000;}void WriteBlockUnsafe(u64 a,const u8*p,size_t n){for(size_t i=0;i<n;++i)output[a+i]=p[i];}};
struct Info{size_t size;};
struct Image{size_t unswizzled_size_bytes;u64 gpu_addr,cpu_addr;size_t guest_size_bytes;Info info;ImageFlagBits flags{};bool partial;u8 value;
 void DownloadMemory(Map&map,int){std::fill_n(map.mapped_span.begin()+map.offset,unswizzled_size_bytes,value);}
};using ImageBase=Image;using AsyncBuffer=Map;
int FullDownloadCopies(const Info&){return 0;}
void SwizzleImage(Memory&mem,u64 a,Info info,int,std::span<u8>bytes,int&){for(size_t i=0;i<info.size;++i)mem.output[a+i]=bytes[i];}
struct Runtime{Map DownloadStagingBuffer(size_t n,bool=true){auto d=std::make_shared<std::vector<u8>>(n,0xcc);return {d,*d,0};}};
struct Tracker{u64 Serial()const{return 17;}};
template<class P>struct TextureCache{
 struct PendingDownload{bool is_swizzle;size_t async_buffer_id,object_id;u64 observation_serial{};bool observed_partial{};size_t observed_address_space{};};
 struct BufferDownload{u64 address;size_t size;};
 struct Owner{Memory*gpu_memory;};
 Runtime runtime;std::vector<Image>slot_images;std::map<size_t,BufferDownload>slot_buffer_downloads;
 Tracker readback_pages;std::vector<PendingDownload>uncommitted_downloads;std::deque<std::vector<PendingDownload>>committed_downloads;
 std::vector<Map>uncommitted_async_buffers,async_buffers_death_ring;std::deque<std::vector<Map>>async_buffers;
 Memory bound,original;Memory*gpu_memory=&bound;std::map<size_t,Owner>address_spaces{{1,{&original}}};int swizzle_data_buffer=0;unsigned partial_publications=0,ordinary_publications=0;
 void DeduplicateDownloads(){}
 void CommitAsyncFlushes();void PopAsyncFlushes();
 bool TryDownloadObservedImage(Image&im,Map&map,PendingDownload&d){if(!im.partial)return false;d.observed_partial=true;d.observed_address_space=1;map.mapped_span[map.offset+3]=im.value;return true;}
 bool PublishObservedReadback(const Image&im,int,std::span<u8>bytes,u64 serial,Memory*owner=nullptr){check(serial==17,"commit observation serial retained");if(!owner)return false;check(owner==&original,"partial publication uses original address space after channel switch");check(bytes[3]==im.value,"partial source uses image slot offset");owner->output[im.gpu_addr+3]=bytes[3];++partial_publications;return true;}
};
'''+commit+pop+r'''
int main(){unsigned cases=0;
for(unsigned seed=0;seed<200;++seed){TextureCache<int>t;unsigned expected_partial=0;
 // A DMA allocation before the shared image allocation exercises distinct buffer IDs.
 auto dma=t.runtime.DownloadStagingBuffer(17,true);dma.offset=7;dma.mapped_span[7]=0x8a;t.uncommitted_async_buffers.push_back(dma);t.slot_buffer_downloads[0]={0xf00000,1};t.uncommitted_downloads.push_back({false,0,0});
 const unsigned n=seed%66;
 for(unsigned i=0;i<n;++i){size_t size=65+(i%5)*17;u64 a=0x1000+i*4096;bool partial=(i+seed)%3!=0;bool remapped=(i+seed)%7==0;Image im{size,a,a+0x100000,size,{size},ImageFlagBits(remapped?1:0),partial,u8(i+7)};t.slot_images.push_back(im);t.uncommitted_downloads.push_back({true,0,i});expected_partial+=partial&&!remapped;}
 t.CommitAsyncFlushes();check(t.uncommitted_downloads.empty()&&t.committed_downloads.size()==1&&t.async_buffers.front().size()==(n?2:1),"commit keeps independent buffers alive");
 t.PopAsyncFlushes();check(t.committed_downloads.empty()&&t.async_buffers.empty(),"retirement consumes exactly one slot");check(t.partial_publications==expected_partial,"only surviving partial images use observed publication");check(t.bound.output[0xf00000]==0x8a,"DMA offset and lifetime retained");
 for(const auto&im:t.slot_images){if(!False(im.flags&ImageFlagBits::Remapped))continue;if(im.partial){check(t.original.output[im.gpu_addr+3]==im.value,"partial byte published to original owner");check(!t.bound.output.contains(im.gpu_addr),"partial staging never reaches full-swizzle fallback");}else for(size_t i=0;i<im.info.size;++i)check(t.bound.output[im.gpu_addr+i]==im.value,"ordinary full image keeps full publication");}
 t.CommitAsyncFlushes();t.PopAsyncFlushes();check(t.committed_downloads.empty(),"empty logical slots retire");++cases;
}
printf("PASS %u production async queue cases: mixed full/partial/DMA, strided offsets, captured serial, channel switch, remap and empty slots\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-copy-queue-')as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

 # Negative control: accidentally treating a partial buffer as a full image
 # must be caught by the queue oracle, even when the observed route succeeded.
 mutation=code.replace('} else if (!PublishObservedReadback', '} if (!PublishObservedReadback',1)
 assert mutation!=code
 (p/'bad.cpp').write_text(mutation)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined',str(p/'bad.cpp'),'-o',str(p/'bad')],check=True)
 result=subprocess.run([str(p/'bad')],capture_output=True,text=True)
 assert result.returncode==99,(result.returncode,result.stdout,result.stderr)
 print('PASS negative control rejects full-swizzle fallback from partially initialized staging')
