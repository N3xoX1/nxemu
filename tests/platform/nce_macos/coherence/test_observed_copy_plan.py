#!/usr/bin/env python3
"""GPU rectangle coverage plus production publication vs an independent tiled-byte oracle."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
code=r'''
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "yuzu_video_core/texture_cache/observed_readback_plan.h"
#include "yuzu_video_core/texture_cache/readback_page_tracker.h"
#include "yuzu_video_core/textures/swizzle_range.h"
using namespace VideoCommon;
void check(bool b,const char*s){if(!b){fprintf(stderr,"FAIL %s\n",s);exit(99);}}
u64 tiled(u64 x,u64 y,u64 row,u32 bh){u64 g=(row+63)/64;return (y/(8ULL<<bh))*g*(512ULL<<bh)+(x/64)*(512ULL<<bh)+((y/8)%(1ULL<<bh))*512+(x%64/32)*256+(y%8/2)*64+(x%32/16)*32+(y%2)*16+x%16;}
int main(){std::mt19937 rng(130);unsigned cases=0,sparse=0;
for(u32 width:{1U,3U,16U,17U,63U,64U,65U,320U,512U,1600U})for(u32 height:{1U,7U,8U,9U,65U,512U})for(u32 bh:{0U,1U,3U,5U})for(unsigned pattern=0;pattern<4;++pattern){
 const u32 row=width*4;const u64 total=((u64(height)+(8ULL<<bh)-1)/(8ULL<<bh))*((u64(row)+63)/64)*(512ULL<<bh),physical=0x100003;
 ReadbackPageTracker tracker;
 if(pattern==0)tracker.Observe(physical,1);
 if(pattern==1)for(unsigned i=0;i<8;++i)tracker.Observe(physical+rng()%total,1+rng()%128);
 if(pattern==2)tracker.Observe(physical,total);
 if(pattern==3)for(unsigned i=0;i<4;++i)tracker.Observe(physical+rng()%total,1);
 const auto serial=tracker.Serial();std::vector<std::pair<u64,u64>> ranges;
 tracker.ForEachRange(physical,total,serial,[&](u64 a,u64 n){ranges.emplace_back(a,n);});
 auto plan=PlanObservedReadback(row,height,bh,ranges);sparse+=plan.has_value();
 std::vector<u8> gpu(u64(row)*height),staging(gpu.size(),0x5c),out(total,0xa7),expected=out;
 for(auto&b:gpu)b=rng();
 if(plan){check(plan->size()<=64,"bounded rectangle count");for(auto r:*plan){check(r.width&&r.height&&r.x+r.width<=width&&r.y+r.height<=height,"GPU rectangles in bounds");for(u32 y=r.y;y<r.y+r.height;++y)memcpy(staging.data()+u64(y)*row+r.x*4,gpu.data()+u64(y)*row+r.x*4,r.width*4);}}
 else staging=gpu;
 // A late observation and a newer synchronous partial read must not consume
 // uninitialized staging or be overwritten by an older asynchronous generation.
 if(pattern==3){tracker.Observe(physical+total/2,1);tracker.MarkSynchronized(physical+7,19);}
 ranges.clear();tracker.ForEachRange(physical,total,serial,[&](u64 a,u64 n){ranges.emplace_back(a,n);Tegra::Texture::Swizzle2DRange(std::span(out).subspan(a,n),staging,row,height,bh,a);});
 for(u64 y=0;y<height;++y)for(u64 x=0;x<row;++x){const auto a=tiled(x,y,row,bh);for(auto[o,n]:ranges)if(o<=a&&a-o<n){expected[a]=gpu[y*row+x];break;}}
 check(out==expected,"planned GPU copies publish exactly the independent full-image byte oracle");++cases;
}
std::array<std::pair<u64,u64>,1> page{{{0,4096}}};auto plan=PlanObservedReadback(512*4,512,4,page);check(plan.has_value(),"sparse 512-square plan");u64 bytes=0;for(auto r:*plan)bytes+=u64(r.width)*r.height*4;check(bytes==4096,"one observed page copies 4 KiB instead of 1 MiB");
check(!PlanObservedReadback(0,512,4,page),"invalid row rejected");check(!PlanObservedReadback(2048,512,32,page),"invalid block height rejected");
std::array<std::pair<u64,u64>,1> overflow{{{~u64{0}-10,20}}};check(!PlanObservedReadback(2048,512,4,overflow),"overflow rejected");
printf("PASS %u layouts/patterns (%u sparse plans), independent byte oracle, late observation/newer sync, dense fallback; 512x512 one page: %llu bytes vs 1048576\n",cases,sparse,(unsigned long long)bytes);
}
'''
with tempfile.TemporaryDirectory(prefix='nce-copy-plan-') as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(root/'src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
