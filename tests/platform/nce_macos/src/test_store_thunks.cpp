// Execute production store handlers on ARM64 against fragmented 4 KiB backing.
#include <array>
#include <atomic>
#include <cstring>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include "nce/store_thunk.h"
#include "nce_runtime.h"
#include "report.h"
#include "state.h"
#include "suite.h"
namespace ncev { namespace {
using u64=std::uint64_t;using u32=std::uint32_t;
namespace S=Core::NCE::StoreThunk;
constexpr u64 Page=4096,Host=16384;
struct Memory {
 u64 guest,stack,*table;std::array<u64,4> physical;
 std::array<bool,4> allowed,dirty;unsigned traps=0,faults=0,downloads=0;
 const GuestImage* image;
 bool NceAccessAllowed(u64 a,bool)const{return a>=guest&&a-guest<Host&&allowed[(a-guest)/Page];}
 void Publish(unsigned p){table[(guest+p*Page)>>12]=((physical[p]-(guest+p*Page))&~u64{4095})|(allowed[p]&&!dirty[p]?3:0);}
 void NceTrackWrites(u64 a){if(a<guest||a-guest>=Host)return;auto p=(a-guest)/Page;if(dirty[p]){++downloads;dirty[p]=false;for(unsigned b=0;b<Page;++b)*reinterpret_cast<unsigned char*>(physical[p]+b)^=0xa7;Publish(p);}}
 bool NcePrepareCanonicalWrite(u64 a){NceTrackWrites(a);Publish((a-guest)/Page);return NceAccessAllowed(a,true);}
 unsigned char* GetPointerSilent(u64 a){return reinterpret_cast<unsigned char*>(physical[(a-guest)/Page]+(a-guest)%Page);}
 void Write8(u64 a,unsigned char v){*GetPointerSilent(a)=v;}
 u64 Read64(u64 a){u64 v;memcpy(&v,reinterpret_cast<void*>(a),8);return v;}
};
struct Registers{Mcontext* c;u64& operator[](size_t n){return Gpr(c,int(n));}};
bool Fault(Vcpu& cpu,int sig,siginfo_t*,Mcontext* c){auto&m=*static_cast<Memory*>(cpu.user);u64 pc=c->__ss.__pc;
 if(sig==SIGTRAP){auto word=*reinterpret_cast<u32*>(pc);if(!S::IsTrap(word))return false;++m.traps;u64 original;u32 instruction;memcpy(&original,reinterpret_cast<void*>(pc+4),8);memcpy(&instruction,reinterpret_cast<void*>(pc+12),4);Registers r{c};S::Complete(m,r,c->__ss.__sp,c->__ss.__pc,original,instruction,S::Trap((word>>5)&65535),c->__ns.__v);return true;}
 ++m.faults;auto it=m.image->restart_pcs.find(pc);if(it!=m.image->restart_pcs.end()){c->__ss.__pc=it->second;return true;}return false;
}
u64* Table(){constexpr u64 n=1ULL<<30;auto raw=mmap(nullptr,n*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);if(raw==MAP_FAILED)abort();u64 start=reinterpret_cast<u64>(raw),base=(start+n-1)&~(n-1);if(base>start)munmap(raw,base-start);if(base+n<start+2*n)munmap(reinterpret_cast<void*>(base+n),start+2*n-base-n);return reinterpret_cast<u64*>(base);}
void Tests(){Memory m{};m.table=Table();m.guest=WindowAlloc(Host,PROT_NONE);m.stack=WindowAlloc(Host*4,PROT_READ|PROT_WRITE);for(auto&p:m.physical)p=WindowAlloc(Host,PROT_READ|PROT_WRITE);
 std::vector<u32> instructions,words;
 auto add=[&](u32 i){instructions.push_back(i);words.push_back(i);words.push_back(0xd4200000);};
 for(unsigned rn=0;rn<31;++rn)for(unsigned rt:{0U,1U,18U,30U,31U}){
 for(unsigned size=0;size<4;++size)for(unsigned off:{0U,1U,4095U})add(0x39000000u|(size<<30)|(off<<10)|(rn<<5)|rt);
 for(unsigned size:{2U,3U}){for(unsigned off:{0U,1U,4095U})add(0x3d000000u|(size<<30)|(off<<10)|(rn<<5)|rt);
 for(unsigned option:{2U,3U,6U,7U})for(unsigned scale:{0U,1U})for(unsigned rm:{0U,8U,31U})add(0x3c200800u|(size<<30)|(rm<<16)|(option<<13)|(scale<<12)|(rn<<5)|rt);}
 for(unsigned off:{0U,1U,63U,64U,127U})add(0xad000000u|(off<<15)|(((rt+1)%32)<<10)|(rn<<5)|rt);
 }
 for(unsigned rn=0;rn<31;++rn)for(u32 op:{0xd50b7e20u,0xd50b7a20u,0xd50b7b20u})add(op|rn);
 GuestImage image;std::string error;bool built=BuildPatchedImage(words,image,error,nullptr,true,false,false,reinterpret_cast<u64>(m.table),false,false,true);
 Expect("W1",built&&image.read_handlers.size()==instructions.size(),Class::Blocker,"Production native store handlers",Fmt("%zu sites %s",instructions.size(),error.c_str()));if(!built)return;m.image=&image;
 WindowProtect(image.base,image.size,PROT_READ|PROT_WRITE);for(auto[pc,target]:image.read_handlers)*reinterpret_cast<u32*>(pc)=0x14000000u|(((target-pc)>>2)&0x3ffffff);WindowProtect(image.base,image.size,PROT_READ|PROT_EXEC);sys_icache_invalidate(reinterpret_cast<void*>(image.base),image.size);
 Vcpu cpu;cpu.BindThread();cpu.user=&m;cpu.fault_hook=Fault;std::mt19937_64 rng(130);auto fp=ProbeFpMasks();unsigned bad=0,cases=0;std::string first;const u64 guard=WindowAlloc(Host,PROT_NONE),readonly=WindowAlloc(Host,PROT_READ);
 for(size_t site=0;site<instructions.size();++site)for(unsigned scenario=0;scenario<11;++scenario){u32 i=instructions[site];unsigned bytes=S::Bytes(i),p=scenario%4;
 std::array<std::array<unsigned char,Page>,4> expected;
 for(unsigned j=0;j<4;++j){for(unsigned b=0;b<Page;++b)expected[j][b]=(b*37+j*113)^(b>>4);memcpy(reinterpret_cast<void*>(m.physical[j]),expected[j].data(),Page);m.allowed[j]=true;m.dirty[j]=false;}
 u64 address=m.guest+p*Page+128;if(scenario==4)address=m.guest+Page-1;
 auto in=RandomState(rng,m.stack+2*Host,fp);in.pc=image.base+image.code_offset+site*8;
 // Indexed offsets include negative/sign-extended and scaled forms. Base/index overlap is defined by the initial register value.
 u64 offset=u64(S::Offset(i));
 if(S::Indexed(i)){unsigned rm=(i>>16)&31,option=(i>>13)&7;if(rm!=31)in.x[rm]=option>=6?u64(-3):3;u64 v=rm==31?0:in.x[rm];if(option==2)v=u32(v);if(option==6)v=u64(std::int64_t(std::int32_t(v)));offset=v<<((i&(1<<12))?(i>>30):0);if(rm==S::Base(i)){in.x[rm]=0;address=0;}}
 if(scenario==8)in.sp=guard+Host;if(scenario==9)in.sp=readonly+Host;
 if(address==0)continue;in.x[S::Base(i)]=address-offset;
 if(scenario==5){m.dirty[p]=true;for(auto&b:expected[p])b^=0xa7;}
 if(scenario==6)m.allowed[p]=false;
 for(unsigned j=0;j<4;++j)m.Publish(j);
 if(scenario==7)m.table[address>>12]&=~u64{2};
 if(scenario==10)m.table[address>>12]=((guard-(address&~u64{4095}))&~u64{4095})|3;
 auto state_expected=in;state_expected.pc+=4;unsigned char payload[32]{};unsigned rt=i&31;
 if(S::Floating(i)){memcpy(payload,in.v[rt],S::Pair(i)?16:bytes);if(S::Pair(i))memcpy(payload+16,in.v[(i>>10)&31],16);}else if(!S::Cache(i)){u64 v=rt==31?0:in.x[rt];memcpy(payload,&v,bytes);}
 if(m.NceAccessAllowed(address,true)&&m.NceAccessAllowed(address+bytes-1,true)&&!S::Cache(i))for(unsigned b=0;b<bytes;++b)expected[(address+b-m.guest)/Page][(address+b-m.guest)%Page]=payload[b];
 if(scenario==5&&S::Cache(i)==false&& !m.NceAccessAllowed(address,true))for(auto&b:expected[p])b^=0xa7;
 unsigned traps=m.traps,faults=m.faults,downloads=m.downloads;
 LoadState(cpu.ctx,in);auto hr=cpu.Run();auto diff=Diff(state_expected,StoreState(cpu.ctx,true));
 for(unsigned j=0;j<4;++j)if(memcmp(expected[j].data(),reinterpret_cast<void*>(m.physical[j]),Page)){++diff.mismatches;diff.text+="bytes ";}
 if(hr!=HrBreakpoint())++diff.mismatches;
 if(scenario<4&&(traps!=m.traps||faults!=m.faults)){++diff.mismatches;diff.text+="fast trap ";}
 if(scenario==5&&m.downloads!=downloads+1){++diff.mismatches;diff.text+="GPU missing ";}
 if(diff.mismatches){++bad;if(first.empty())first=Fmt("i=%08x scenario=%u %s",i,scenario,diff.text.c_str());}++cases;
 }
 Expect("W2",bad==0,Class::Blocker,"Native writes preserve full ARM state and neighboring bytes",Fmt("%u cases, %u failures %s",cases,bad,first.c_str()));

 unsigned interruption_cases=0,interruption_bad=0;bool replay_rejected=false;
 for(u32 op:{0x39028001u,0xad000901u,0xbc2b6991u,0xd50b7e2au}){
  std::vector<u32> code{op,0xd4200000};GuestImage single;
  if(!BuildPatchedImage(code,single,error,nullptr,true,false,false,reinterpret_cast<u64>(m.table),false,false,true)){++interruption_bad;continue;}
  const u64 pc=single.base+single.code_offset,thunk=single.read_handlers.at(pc);
  PatchImageWord(single,pc,0x14000000u|(((thunk-pc)>>2)&0x3ffffff));m.image=&single;
  for(u64 stop=thunk;;stop+=4){u32 original=*reinterpret_cast<u32*>(stop);bool terminal=(original&0xfc000000u)==0x14000000u;
   for(unsigned j=0;j<4;++j){m.allowed[j]=true;m.dirty[j]=false;m.Publish(j);memset(reinterpret_cast<void*>(m.physical[j]),0x56,Page);}
   auto in=RandomState(rng,m.stack+2*Host,fp);in.pc=pc;
   if(S::Indexed(op))in.x[(op>>16)&31]=3;
   in.x[S::Base(op)]=m.guest+128-u64(S::Offset(op));if(S::Indexed(op))in.x[S::Base(op)]-=3;
   auto expected=in;expected.pc=pc+4;
   PatchImageWord(single,stop,0xd4200000);LoadState(cpu.ctx,in);auto hr=cpu.Run();bool reached=hr==HrBreakpoint()&&cpu.ctx.pc==stop;
   PatchImageWord(single,stop,original);
   // The completed-store restart must preserve the bytes already published.
   auto restart=single.restart_pcs.find(stop);bool completed=restart!=single.restart_pcs.end()&&S::IsTrap(*reinterpret_cast<u32*>(restart->second))&&((*reinterpret_cast<u32*>(restart->second)>>5)&65535)==unsigned(S::Trap::Completed);
   std::array<unsigned char,32> saved{};if(completed){memcpy(saved.data(),m.GetPointerSilent(m.guest+128),S::Bytes(op));if(!S::Cache(op)){for(unsigned j=0;j<S::Bytes(op);++j)saved[j]^=0x5b;memcpy(m.GetPointerSilent(m.guest+128),saved.data(),S::Bytes(op));}}
   auto stopped=StoreState(cpu.ctx,true);
   if(reached){if(restart!=single.restart_pcs.end())cpu.ctx.pc=restart->second;hr=cpu.Run();}
   bool failed=!reached||hr!=HrBreakpoint()||Diff(expected,StoreState(cpu.ctx,true)).mismatches;
   if(completed&&memcmp(saved.data(),m.GetPointerSilent(m.guest+128),S::Bytes(op)))failed=true;
   if(completed&&!S::Cache(op)){
    // Deliberately use the wrong restart kind: byte comparison must catch replay.
    u64 resumed_pc=stop;S::Complete(m,stopped.x,stopped.sp,resumed_pc,pc,op,S::Trap::Spilled,stopped.v);
    replay_rejected|=memcmp(saved.data(),m.GetPointerSilent(m.guest+128),S::Bytes(op))!=0;
   }
   interruption_bad+=failed;++interruption_cases;if(terminal)break;
  }
 }
 Expect("W3",interruption_bad==0,Class::Blocker,"Interruptions at every native store instruction",Fmt("%u points, %u failures",interruption_cases,interruption_bad));
 Expect("W4",replay_rejected,Class::Blocker,"Negative control rejects replay of a completed store","");
 munmap(m.table,1ULL<<30);
}
}void RunStoreThunkTests(){Tests();}}
