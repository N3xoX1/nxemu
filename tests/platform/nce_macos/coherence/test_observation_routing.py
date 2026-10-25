#!/usr/bin/env python3
"""Exercise the actual flush-area method; unknown neighboring pages must synchronize."""
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[4]
s=(ROOT/'src/yuzu_video_core/texture_cache/texture_cache.h').read_text();a=s.index('template <class P>\nstd::optional<RasterizerDownloadArea> TextureCache<P>::GetFlushArea(');b=s.index('\ntemplate <class P>',a+10)
code=r'''
#include <algorithm>
#include <optional>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include "yuzu_video_core/texture_cache/readback_page_tracker.h"
using VideoCommon::ReadbackPageTracker;using DAddr=u64;using ImageId=unsigned;
namespace Common{template<class T>T AlignUp(T a,T b){return (a+b-1)&~(b-1);}template<class T>T AlignDown(T a,T b){return a&~(b-1);}}
enum class ImageFlagBits:u32{GpuModified=1};bool False(ImageFlagBits f){return u32(f)==0;}ImageFlagBits operator&(ImageFlagBits a,ImageFlagBits b){return ImageFlagBits(u32(a)&u32(b));}
enum class ImageViewFlagBits:u32{PreemtiveDownload=1};ImageViewFlagBits&operator|=(ImageViewFlagBits&a,ImageViewFlagBits b){return a=ImageViewFlagBits(u32(a)|u32(b));}
struct ImageBase{ImageFlagBits flags{1};u64 cpu_addr,cpu_addr_end;std::vector<unsigned>image_view_ids;struct Info{bool forced_flushed=false;}info;};
struct View{ImageViewFlagBits flags{0};};struct RasterizerDownloadArea{u64 startAddress=0,endAddress=0;bool preemtive=false;};
template<class P>struct TextureCache{
 std::vector<ImageBase>images;std::vector<View>slot_image_views{3};ReadbackPageTracker readback_pages;u64 queried_addr=0,queried_size=0;
 template<class F>void ForEachImageInRegion(u64 a,u64 n,F f){queried_addr=a;queried_size=n;for(auto&im:images)if(im.cpu_addr<a+n&&a<im.cpu_addr_end)f(0,im);}
 std::optional<RasterizerDownloadArea>GetFlushArea(DAddr,u64);
};
void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(99);}}
'''+s[a:b]+r'''
int main(){
 bool observed=std::getenv("NXEMU_NCE_OBSERVED_READBACK")!=nullptr;
 TextureCache<int>t;t.images={{ImageFlagBits(1),0x4000,0x4080,{0},{}},{ImageFlagBits(1),0x4800,0x4c00,{1},{}},{ImageFlagBits(1),0x4000,0xc000,{2},{}}};
 auto first=t.GetFlushArea(0x4010,4);require(first&&!first->preemtive,"first image read synchronizes");
 if(observed){
  require(t.queried_addr==0x4000&&t.queried_size==4096,"scalar read initializes all dirty textures sharing its guest page");
  require(first->startAddress==0x4000&&first->endAddress==0x5000,"OS caches requested page, never entire image");
  require(u32(t.slot_image_views[1].flags)==1,"disjoint alias in same guest page is included");
  auto same=t.GetFlushArea(0x4ffe,2);require(same&&same->preemtive,"other CPU reads of initialized page use async path");
  auto neighbor=t.GetFlushArea(0x5010,4);require(neighbor&&!neighbor->preemtive,"new 4K page inside same 16K host page synchronizes despite forced image");
  require(neighbor->startAddress==0x5000&&neighbor->endAddress==0x6000,"new neighbor gets independent OS range");
  require(t.GetFlushArea(0x5011,1)->preemtive,"neighbor becomes preemptive after first read");
  auto crossing=t.GetFlushArea(0x5fff,2);require(crossing&&!crossing->preemtive&&t.queried_size==8192,"cross-page reads initialize both pages");
  t.images[2].info.forced_flushed=false;require(!t.GetFlushArea(0x5011,1)->preemtive,"new image ownership still requires its first synchronization");
 }else{
  require(first->endAddress==0xc000,"baseline whole-image area preserved");
  require(t.GetFlushArea(0x5010,4)->preemtive,"baseline image-wide preemptive policy preserved");
 }
 require(!t.GetFlushArea(0x100000,4),"no texture produces no area");
 puts(observed?"PASS observed production routing: whole guest page, four host neighbors, first/new/shared/cross-page reads, new images":"PASS legacy production routing unchanged");
}
'''
with tempfile.TemporaryDirectory(prefix='nce-routing-')as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 for mode in (False,True):
  env=dict(os.environ);env.pop('NXEMU_NCE_OBSERVED_READBACK',None)
  if mode:env['NXEMU_NCE_OBSERVED_READBACK']='1'
  subprocess.run([str(p/'test')],env=env,check=True)
