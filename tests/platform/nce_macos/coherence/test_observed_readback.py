#!/usr/bin/env python3
"""Test production page epochs and range conversion against a byte-address oracle."""
from pathlib import Path
import subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[4]
prefix=r'''
#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "yuzu_video_core/texture_cache/readback_page_tracker.h"
#include "yuzu_video_core/textures/swizzle_range.h"
using VideoCommon::ReadbackPageTracker;
void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(99);}}
// Independent Tegra byte-address oracle, retaining untouched padding/CPU bytes.
u64 offset(u64 x,u64 y,u32 row_bytes,u32 bh){
 u64 gobs=(u64(row_bytes)+63)/64;
 return (y/(8ULL<<bh))*gobs*(512ULL<<bh)+(x/64)*(512ULL<<bh)+
        ((y/8)% (1ULL<<bh))*512+(x%64/32)*256+(y%8/2)*64+(x%32/16)*32+(y%2)*16+x%16;
}
std::vector<std::pair<u64,u64>>ranges(const ReadbackPageTracker&t,u64 addr,u64 size,u64 stamp){
 std::vector<std::pair<u64,u64>>r;t.ForEachRange(addr,size,stamp,[&](u64 a,u64 n){r.emplace_back(a,n);});return r;
}
int main(){
 ReadbackPageTracker t;
 require(!t.Observe(0x4000,4),"first read needs synchronization");u64 old=t.Serial();
 require(t.Observe(0x4004,8),"another access in the same 4K page is known");
 require(!t.Observe(0x5000,1),"a 4K neighbor in the same 16K host page still needs initialization");
 require(ranges(t,0x4000,0x4000,old)==std::vector<std::pair<u64,u64>>{{0,4096}},"old snapshot cannot initialize newly observed neighbor");
 t.MarkSynchronized(0x4000,0x4000);
 require(ranges(t,0x4000,0x4000,old).empty(),"old asynchronous snapshot cannot overwrite a newer complete readback, including aliases");
 require(ranges(t,0x4000,0x4000,t.Serial())==std::vector<std::pair<u64,u64>>{{0,8192}},"new snapshots publish both observed pages");
 old=t.Serial();t.Forget(0x4000,4096);require(!t.Observe(0x4000,1),"remapped physical page is a new observation");
 require(ranges(t,0x4000,4096,old).empty(),"retired mapping snapshot cannot initialize replacement");
 ReadbackPageTracker partial;
 partial.Observe(0x8000,4096);old=partial.Serial();partial.MarkSynchronized(0x8123,19);
 require(ranges(partial,0x8000,4096,old)==std::vector<std::pair<u64,u64>>{{0,0x123},{0x136,4096-0x136}},"partial-page alias excludes only newer bytes; neighbors still published");
 for(unsigned i=0;i<5000;++i)partial.MarkSynchronized(0x8123,19);
 require(ranges(partial,0x8000,4096,old)==std::vector<std::pair<u64,u64>>{{0,0x123},{0x136,4096-0x136}},"repeated partial publications preserve stable byte boundaries");
 // Reproduce fragmented 16K host pages, dense cache commands and four CPU streams.
 unsigned new_reads=0;
 for(unsigned frame=0;frame<200;++frame)for(unsigned command=0;command<40;++command)for(unsigned core=0;core<4;++core){
  if(!t.Observe(0x100000+core*4096,16))++new_reads;
 }
 require(new_reads==4,"32000 dense reads create four observations, not one global rearm per command");
 unsigned cases=0;std::mt19937 rng(0x5a17);
 for(u32 width:{1U,3U,7U,16U,63U,64U,65U,320U,1600U,1920U})
 for(u32 height:{1U,7U,8U,9U,65U,900U})for(u32 bh:{0U,1U,3U,5U}){
  const u32 row=width*4;u64 total=((u64(height)+(8ULL<<bh)-1)/(8ULL<<bh))*((u64(row)+63)/64)*(512ULL<<bh);
  std::vector<u8>input(u64(row)*height);for(auto&x:input)x=rng();
  std::vector<u8>out(total,0xa7),expected=out;
  ReadbackPageTracker pages;
  // Deliberately unaligned allocation, page edges, holes and clipped last page.
  const u64 physical=0x100003;
  for(unsigned i=0;i<12;++i){u64 start=rng()%total;pages.Observe(physical+start,1+rng()%97);}
  auto selected=ranges(pages,physical,total,pages.Serial());
  for(auto [a,n]:selected)Tegra::Texture::Swizzle2DRange(std::span(out).subspan(a,n),input,row,height,bh,a);
  for(u64 y=0;y<height;++y)for(u64 x=0;x<row;++x){u64 dst=offset(x,y,row,bh);bool wanted=false;for(auto[a,n]:selected)wanted|=a<=dst&&dst<a+n;if(wanted)expected[dst]=input[y*row+x];}
  require(out==expected,"selected pages byte-exact; untouched pages, padding and partial CPU stores preserved");
  // Cover a whole image in unaligned pieces, including slices through 16-byte sectors.
  std::fill(out.begin(),out.end(),0xa7);std::fill(expected.begin(),expected.end(),0xa7);
  for(u64 a=0;a<total;){u64 n=std::min<u64>(1+rng()%8191,total-a);Tegra::Texture::Swizzle2DRange(std::span(out).subspan(a,n),input,row,height,bh,a);a+=n;}
  for(u64 y=0;y<height;++y)for(u64 x=0;x<row;++x)expected[offset(x,y,row,bh)]=input[y*row+x];
  require(out==expected,"range conversion composes to full image with exact padding");++cases;
 }
 printf("PASS %u conversion layouts, selected/full ranges, 32000 dense four-core observations, host-page neighbors, aliases, old async copies and remap generations\n",cases);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-observed-')as directory:
 p=Path(directory);(p/'test.cpp').write_text(prefix)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(ROOT/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
 if '--negative-controls' in sys.argv:
  header=ROOT/'src/yuzu_video_core/texture_cache/readback_page_tracker.h'
  mutations={
   'old_snapshot_initializes_new_page':('if (it->second.observed > snapshot_serial)', 'if (false)'),
   'old_snapshot_overwrites_new_sync':('if (span.serial > snapshot_serial)', 'if (false)'),
  }
  target=p/'yuzu_video_core/texture_cache/readback_page_tracker.h';target.parent.mkdir(parents=True)
  for name,(old,new) in mutations.items():
   text=header.read_text();assert old in text;target.write_text(text.replace(old,new,1))
   subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'src'),str(p/'test.cpp'),'-o',str(p/'bad')],check=True)
   result=subprocess.run([str(p/'bad')],capture_output=True,text=True)
   assert result.returncode==99,(name,result.returncode,result.stdout,result.stderr)
   print('PASS negative control rejected:',name)
