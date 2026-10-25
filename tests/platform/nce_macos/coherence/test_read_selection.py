#!/usr/bin/env python3
"""Production extra-read selector: bounded budget, baseline coverage and deterministic ranking."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[4]
s=(root/'src/nxemu-cpu/nce/patcher.cpp').read_text()
a=s.index('    // Keep the basic X0 coverage');b=s.index('    // Loop through instructions',a)
selection=s[a:b]
code=r'''
#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>
#include <span>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include "nce/read_thunk.h"
#include "nce/store_thunk.h"
using u32=uint32_t;using u64=uint64_t;
namespace ReadThunk=Core::NCE::ReadThunk;
namespace StoreThunk=Core::NCE::StoreThunk;
// Scale the pool only; preserve the real production handler-size bound and algorithm.
constexpr size_t operator""_MiB(unsigned long long n){return n*100;}
constexpr size_t ModuleCodeIndex=9,MaxReadHandlerBytes=160;
using s8=int8_t;constexpr size_t DataWindowWords=1024;
// The test texts hold only code: data detection is covered by A3.24.
static bool IsDataWindow(std::span<const u32>,size_t){return false;}
void check(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
std::vector<bool>select(std::span<const u32>text_words,size_t m_extra_read_bytes,bool read_thunks=true,bool all_read_bases=true,u64 atomic_table=1,std::span<const u32>read_filter={},bool float_reads=false){
bool store_thunks=false;size_t m_store_bytes=0;
'''+selection+r'''
return extra_reads;
}
int main(){
 const u32 basic=0xb9400008,common=0xb9400109,rare=0xb940092a,overlap=0xb940014a;
 std::vector<u32>words(9,0xd503201f);words.push_back(rare);
 for(unsigned i=0;i<20;++i){words.push_back(common);words.push_back(basic);}
 auto selected=select(words,0);unsigned count=0;
 for(unsigned i=9;i<words.size();++i){count+=selected[i];if(selected[i])check(words[i]==common,"frequency outranks early rare site and ignores basic coverage");}
 check(count==10,"exact independent extra budget");check(!selected[9],"early rare instruction cannot consume common-site budget");
 for(size_t used:{0U,1U,159U,160U,1599U,1600U,1601U,99999U}){
  auto result=select(words,used);size_t n=0;for(auto bit:result)n+=bit;
  check(n==(1600-std::min(used,size_t{1600}))/160,"rounding and exhausted/oversized budgets bounded");
 }
 for(unsigned option=0;option<3;++option){auto result=select(words,0,option!=0,option!=1,option==2?0:1);for(auto bit:result)check(!bit,"disabled mode or unavailable table emits no extras");}
 std::vector<u32>tie(9,0xd503201f);tie.push_back(overlap);tie.push_back(rare);
 auto result=select(tie,1440);check(result[9]&&!result[10],"ties stable; overlapping base/output supported");
 std::array<u32,1>filter{rare};auto filtered=select(words,0,true,true,1,filter);
 check(filtered[9],"observed rare instruction admitted ahead of statically common unobserved code");
 for(unsigned i=10;i<words.size();++i)check(!filtered[i],"filter excludes unobserved extras and does not consume basic budget");
 std::vector<u32>floats(9,0xd503201f);floats.push_back(0xbd4f5583);floats.push_back(common);
 auto float_result=select(floats,0,true,false,1,{},true);
 check(float_result[9]&&!float_result[10],"FP gate adds only FP forms when integer extension disabled");
 check(!ReadThunk::Basic(0xbd400008),"FP load from X0 cannot enter basic integer route");
 check(!ReadThunk::Eligible(0xb94003e8),"SP remains on interpreter route");
 check(!ReadThunk::Eligible(0xb940001f),"XZR remains on interpreter route");
 puts("PASS production selector: ranking, independent baseline, deterministic ties, overlap, budget edges and opt-in/table gates");
}
'''
with tempfile.TemporaryDirectory(prefix='nce-read-selection-')as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['clang++','-std=c++20','-O1','-g','-fsanitize=address,undefined','-I'+str(root/'src/nxemu-cpu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
