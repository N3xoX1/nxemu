// Real generated ARM64 code, real 16 KiB host faults, independent per-4 KiB byte oracle.
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include "nce/patcher.h"
#include "nce/read_thunk.h"
#include "nce_runtime.h"
#include "report.h"
#include "state.h"
#include "suite.h"

namespace ncev {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;
namespace Read = Core::NCE::ReadThunk;
constexpr u64 Page = 4096, HostPage = 16384;

struct ReadMemory {
    u64 guest{}, stack{};
    std::array<u64, 4> physical{};
    std::array<bool, 4> allowed{true,true,true,true};
    std::array<bool, 4> dirty{};
    u64* table{};
    unsigned traps{}, faults{}, downloads{};
    u64 stack_alias{}, stack_alias_value{};
    const GuestImage* image{};
    bool NceAccessAllowed(u64 a, bool) const {
        return a >= guest && a - guest < HostPage && allowed[(a-guest)/Page];
    }
    void NceTrackReads(u64 a) {
        const unsigned p=(a-guest)/Page;
        if (dirty[p]) {
            ++downloads; dirty[p]=false;
            // Simulate a completed GPU publication changing every byte of this 4 KiB page.
            auto* bytes=reinterpret_cast<std::uint8_t*>(physical[p]);
            for (unsigned i=0;i<Page;++i) bytes[i]^=0xa7;
            Publish(p);
        }
    }
    std::uint8_t Read8(u64 a) const {
        return *reinterpret_cast<const std::uint8_t*>(physical[(a-guest)/Page]+((a-guest)%Page));
    }
    u64 Read64(u64 a) const {if(a==stack_alias)return stack_alias_value;u64 v;std::memcpy(&v,reinterpret_cast<const void*>(a),8);return v;}
    void Publish(unsigned p) {
        const u64 delta=physical[p]-(guest+p*Page);
        std::atomic_ref<u64>(table[(guest+p*Page)>>12]).store(
            (delta & ~u64{4095}) | (allowed[p] && !dirty[p] ? 1 : 0),std::memory_order_release);
    }
    u64 Value(u64 address,unsigned n) const {
        u64 v=0;for(unsigned i=0;i<n;++i)v|=u64{Read8(address+i)}<<(8*i);return v;
    }
};

struct SignalRegisters {
    Mcontext* context;
    u64& operator[](size_t index) { return Gpr(context, static_cast<int>(index)); }
};

bool ReadFault(Vcpu& cpu,int sig,siginfo_t*,Mcontext* mc) {
    auto& m=*static_cast<ReadMemory*>(cpu.user);
    const u64 pc=mc->__ss.__pc;
    if(sig==SIGTRAP) {
        const u32 instruction=*reinterpret_cast<const u32*>(pc);
        if(!Read::IsTrap(instruction))return false;
        ++m.traps;
        u64 original_pc;u32 original;
        std::memcpy(&original_pc,reinterpret_cast<const void*>(pc+4),8);
        std::memcpy(&original,reinterpret_cast<const void*>(pc+12),4);
        if(!Read::Eligible(original))return false;
        SignalRegisters registers{mc};
        Read::Complete(m,registers,mc->__ss.__sp,mc->__ss.__pc,original_pc,original,
                       ((instruction>>5)&0xffff)!=Read::PlainTrap,
                       ((instruction>>5)&0xffff)==Read::CompletedTrap,mc->__ns.__v);
        return true;
    }
    ++m.faults;
    const auto it=m.image->restart_pcs.find(pc);
    if(it!=m.image->restart_pcs.end() && Read::IsTrap(*reinterpret_cast<const u32*>(it->second))) {
        mc->__ss.__pc=it->second;return true;
    }
    return false;
}

u64* AllocateTable() {
    constexpr u64 size=1ULL<<30;
    auto* raw=mmap(nullptr,2*size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(raw==MAP_FAILED)std::abort();
    const u64 base=reinterpret_cast<u64>(raw),aligned=(base+size-1)&~(size-1);
    if(aligned>base)munmap(raw,aligned-base);
    if(aligned+size<base+2*size)munmap(reinterpret_cast<void*>(aligned+size),base+2*size-aligned-size);
    return reinterpret_cast<u64*>(aligned);
}

void Tests() {
    ReadMemory memory;
    memory.table=AllocateTable();
    memory.guest=WindowAlloc(HostPage,PROT_NONE);
    memory.stack=WindowAlloc(4*HostPage,PROT_READ|PROT_WRITE);
    for(unsigned p=0;p<4;++p) {
        memory.physical[p]=WindowAlloc(HostPage,PROT_READ|PROT_WRITE);
        for(unsigned i=0;i<Page;++i)*reinterpret_cast<std::uint8_t*>(memory.physical[p]+i)=
            static_cast<std::uint8_t>((i*37+p*113)^(i>>4));
        memory.Publish(p);
    }
    struct Site{unsigned size,rn,rt,offset;u64 index;bool floating=false;};
    std::vector<Site> sites;std::vector<u32> words;
    for(unsigned size=0;size<4;++size)for(unsigned rn=0;rn<31;++rn)for(unsigned rt=0;rt<31;++rt)
        for(unsigned offset:{0U,1U,10U,4095U}) {
            sites.push_back({size,rn,rt,offset,words.size()});
            words.push_back(0x39400000u|(size<<30)|(offset<<10)|(rn<<5)|rt);
            words.push_back(0xd4200000); // final BRK (not a thunk slow trap)
        }
    for(unsigned size : {2U,3U})for(unsigned rn=0;rn<31;++rn)for(unsigned rt=0;rt<32;++rt)
        for(unsigned offset:{0U,1U,10U,4095U}) {
            sites.push_back({size,rn,rt,offset,words.size(),true});
            words.push_back(0x3d400000u|(size<<30)|(offset<<10)|(rn<<5)|rt);
            words.push_back(0xd4200000);
        }
    GuestImage image;std::string error;std::vector<u32> patched;
    bool built=BuildPatchedImage(words,image,error,&patched,true,false,false,
                                reinterpret_cast<u64>(memory.table),true,true);
    Expect("R1",built && image.read_handlers.size()==sites.size(),Class::Blocker,
           "Native read trampolines generated by the real patcher",error);
    if(!built)return;
    memory.image=&image;
    bool dormant=true;for(const auto&s:sites)dormant&=patched[s.index]==words[s.index];
    Expect("R2",dormant,Class::Blocker,"Direct reads unchanged before specialization",Fmt("%zu sites",sites.size()));
    // Install branches as the process does after the first host fault. Validate execution
    // independently of that process's code-publishing implementation.
    WindowProtect(image.base,image.size,PROT_READ|PROT_WRITE);
    for(const auto&[pc,target]:image.read_handlers)
        *reinterpret_cast<u32*>(pc)=0x14000000u|(((target-pc)>>2)&0x03ffffffu);
    WindowProtect(image.base,image.size,PROT_READ|PROT_EXEC);
    sys_icache_invalidate(reinterpret_cast<void*>(image.base),image.size);
    Vcpu cpu;cpu.BindThread();cpu.fault_hook=ReadFault;cpu.user=&memory;
    std::mt19937_64 rng(0x130f);auto fp=ProbeFpMasks();
    unsigned cases=0,bad=0;std::string first;
    const u64 guard=WindowAlloc(HostPage,PROT_NONE);
    const u64 readonly=WindowAlloc(HostPage,PROT_READ);
    auto run=[&](const Site& site,unsigned scenario) {
        const unsigned bytes=1u<<site.size,p=scenario%4;
        for(unsigned j=0;j<4;++j){memory.allowed[j]=true;memory.dirty[j]=false;memory.Publish(j);}
        u64 address=memory.guest+p*Page+128;
        u64 sp=memory.stack+2*HostPage;
        if(scenario==4)address=memory.guest+Page-1; // unaligned/split page
        if(scenario==5){memory.dirty[p]=true;memory.Publish(p);}
        if(scenario==6){memory.allowed[p]=false;memory.Publish(p);}
        if(scenario==7)sp=guard+HostPage; // spill cannot touch stack guard
        if(scenario==8)sp=readonly+HostPage; // spill on read-only host stack
        if(scenario==9)address=(1ULL<<39)+128; // outside translation table
        if(scenario==10){ // invalid physical alias must fall back to authoritative memory
            memory.table[address>>12]=(guard-(address&~u64{4095}))|1;
        }
        if(scenario==11){address=memory.guest+Page-1;memory.allowed[1]=false;memory.Publish(1);}
        auto in=RandomState(rng,sp,fp);
        in.x[site.rn]=address-u64{site.offset}*bytes;
        const u64 pc=image.base+image.code_offset+site.index*4;
        in.pc=pc;
        // Compute the reference without invoking the production fallback implementation.
        auto expected=in;expected.pc=pc+4;
        bool permitted=memory.NceAccessAllowed(address,false)&&memory.NceAccessAllowed(address+bytes-1,false);
        if(permitted) {
            unsigned p0=(address-memory.guest)/Page,p1=(address+bytes-1-memory.guest)/Page;
            for(unsigned page=p0;page<=p1;++page)if(memory.dirty[page]) {
                auto* data=reinterpret_cast<std::uint8_t*>(memory.physical[page]);
                for(unsigned j=0;j<Page;++j)data[j]^=0xa7;
            }
            const u64 value=memory.Value(address,bytes);
            if(site.floating){expected.v[site.rt][0]=value;expected.v[site.rt][1]=0;}
            else expected.x[site.rt]=value;
            // Undo the oracle's synthetic GPU write: the real slow path must publish it.
            for(unsigned page=p0;page<=p1;++page)if(memory.dirty[page]) {
                auto* data=reinterpret_cast<std::uint8_t*>(memory.physical[page]);
                for(unsigned j=0;j<Page;++j)data[j]^=0xa7;
            }
        }
        const unsigned traps_before=memory.traps,faults_before=memory.faults,downloads_before=memory.downloads;
        LoadState(cpu.ctx,in);const auto hr=cpu.Run();auto got=StoreState(cpu.ctx,true);
        auto diff=Diff(expected,got);
        if(hr!=HrBreakpoint()) {++diff.mismatches;diff.text+="exit="+std::to_string(hr);}
        // Happy path must actually avoid every synchronous fault and trap.
        if(scenario<4&&(memory.traps!=traps_before||memory.faults!=faults_before)) {
            ++diff.mismatches;diff.text+="unexpected fast-path exception";
        }
        if(scenario==5&&memory.downloads!=downloads_before+1){++diff.mismatches;diff.text+="GPU publication missed";}
        if(diff.mismatches){++bad;if(first.empty())first=Fmt("size=%u rn=%u rt=%u off=%u scenario=%u %s",bytes,site.rn,site.rt,site.offset,scenario,diff.text.c_str());}
        ++cases;
    };
    for(const auto&site:sites)for(unsigned scenario=0;scenario<12;++scenario)run(site,scenario);
    Expect("R3",bad==0,Class::Blocker,"1/2/4/8-byte reads: full ARM64 state and protections",
           Fmt("%u cases, %u divergences; GPU, 4 disjoint pages, protected stack, crossed accesses, denied access; %s",cases,bad,first.c_str()));
    // Repeated GPU writes must revoke the fast route on every generation.
    const auto&site=sites[2*31*31*4+8];unsigned before=memory.downloads;
    for(unsigned i=0;i<64;++i)run(site,5);
    Expect("R4",memory.downloads==before+64,Class::Blocker,"New GPU writes revoke the fast path",
           Fmt("%u publications observed",memory.downloads-before));
    // Interrupt at every fast-path instruction, save the entire guest state, then
    // apply exactly the production restart metadata before resuming. This exercises
    // interruption before/after the stack spill, pointer construction and final load.
    const u64 chosen_pc=image.base+image.code_offset+site.index*4;
    const u64 thunk=image.read_handlers.at(chosen_pc);
    unsigned interruption_cases=0,interruption_bad=0;
    for(unsigned selected_rn : {0U,8U,18U,30U})
    for(unsigned selected_rt : {0U,8U,18U,30U})
    for(unsigned selected_offset : {0U,3U}) {
      const auto& interrupted_site=sites[(2*31*31+selected_rn*31+selected_rt)*4+selected_offset];
      const u64 interrupted_pc=image.base+image.code_offset+interrupted_site.index*4;
      const u64 interrupted_thunk=image.read_handlers.at(interrupted_pc);
      for(u64 stop=interrupted_thunk;;stop+=4) {
        const u32 original=*reinterpret_cast<const u32*>(stop);
        const bool terminal=(original&0xfc000000u)==0x14000000u;
        for(unsigned j=0;j<4;++j){memory.allowed[j]=true;memory.dirty[j]=false;memory.Publish(j);}
        auto in=RandomState(rng,memory.stack+2*HostPage,fp);
        const u64 address=memory.guest+128;
        in.x[interrupted_site.rn]=address-u64{interrupted_site.offset}*(1u<<interrupted_site.size);in.pc=interrupted_pc;
        auto expected=in;expected.pc=interrupted_pc+4;expected.x[interrupted_site.rt]=memory.Value(address,1u<<interrupted_site.size);
        PatchImageWord(image,stop,0xd4200000);
        LoadState(cpu.ctx,in);auto hr=cpu.Run();
        bool reached=hr==HrBreakpoint()&&cpu.ctx.pc==stop;
        PatchImageWord(image,stop,original);
        if(reached) {
            auto restart=image.restart_pcs.find(stop);
            if(restart!=image.restart_pcs.end())cpu.ctx.pc=restart->second;
            hr=cpu.Run();
        }
        auto diff=Diff(expected,StoreState(cpu.ctx,true));
        interruption_bad+=!reached||hr!=HrBreakpoint()||diff.mismatches!=0;
        ++interruption_cases;
        if(terminal)break;
    }
    }
    for(unsigned selected_rn : {0U,8U,18U,30U})for(unsigned selected_rt : {0U,3U,31U}) {
      const auto& fsite=sites[4*31*31*4+(selected_rn*32+selected_rt)*4];
      const u64 fpc=image.base+image.code_offset+fsite.index*4;
      const u64 fthunk=image.read_handlers.at(fpc);
      for(u64 stop=fthunk;;stop+=4) {
        const u32 original=*reinterpret_cast<const u32*>(stop);
        const bool terminal=(original&0xfc000000u)==0x14000000u;
        for(unsigned j=0;j<4;++j){memory.allowed[j]=true;memory.dirty[j]=false;memory.Publish(j);}
        auto in=RandomState(rng,memory.stack+2*HostPage,fp);in.pc=fpc;
        in.x[fsite.rn]=memory.guest+128;
        auto expected=in;expected.pc=fpc+4;expected.v[fsite.rt][0]=memory.Value(memory.guest+128,4);expected.v[fsite.rt][1]=0;
        PatchImageWord(image,stop,0xd4200000);LoadState(cpu.ctx,in);auto hr=cpu.Run();
        const bool reached=hr==HrBreakpoint()&&cpu.ctx.pc==stop;PatchImageWord(image,stop,original);
        if(reached){auto restart=image.restart_pcs.find(stop);if(restart!=image.restart_pcs.end())cpu.ctx.pc=restart->second;hr=cpu.Run();}
        interruption_bad+=!reached||hr!=HrBreakpoint()||Diff(expected,StoreState(cpu.ctx,true)).mismatches!=0;
        ++interruption_cases;if(terminal)break;
      }
    }
    Expect("R5",interruption_bad==0,Class::Blocker,"Interrupt and resume at every instruction of the fast path",
           Fmt("%u points, %u divergences (registers, SP, NZCV, vectors)",interruption_cases,interruption_bad));
    // Negative control: removing the GPU-ready test must expose stale bytes. A
    // suite that only exercises the slow fallback would incorrectly accept this.
    u64 permission_test=0;
    for(u64 pc=thunk;pc<thunk+256;pc+=4) {
        if((*reinterpret_cast<const u32*>(pc)&0xfff80000u)==0x36000000u){permission_test=pc;break;}
    }
    bool rejected=false;
    if(permission_test) {
        const u32 original=*reinterpret_cast<const u32*>(permission_test);
        PatchImageWord(image,permission_test,0xd503201f);
        const unsigned previous_bad=bad;
        run(site,5);rejected=bad==previous_bad+1;bad=previous_bad;
        PatchImageWord(image,permission_test,original);
    }
    Expect("R6",rejected,Class::Blocker,"Negative control: a read that ignores the GPU protection is rejected","");
    // Pointer chasing distinguishes a completed overlapping load from a retry:
    // rereading with the new X8 would follow a second pointer and silently change data.
    const auto& pointer_site=sites[(3*31*31+8*31+8)*4];
    const u64 pointer_pc=image.base+image.code_offset+pointer_site.index*4;
    const u64 pointer_thunk=image.read_handlers.at(pointer_pc);
    u64 restore_point=0,spilled_target=0;
    for(const auto&[fault,restart]:image.restart_pcs)if(fault>=pointer_thunk&&fault<pointer_thunk+160) {
        const auto trap=*reinterpret_cast<const u32*>(restart);
        if(trap==(0xd4200000u|(Read::CompletedTrap<<5)))restore_point=fault;
        if(trap==(0xd4200000u|(Read::SpilledTrap<<5)))spilled_target=restart;
    }
    auto pointer_resume=[&](bool wrong_retry,bool protected_restore) {
        for(unsigned j=0;j<4;++j){memory.allowed[j]=true;memory.dirty[j]=false;memory.Publish(j);}
        const u64 first_value=memory.guest+256,second_value=0x51a1e5bad;
        std::memcpy(reinterpret_cast<void*>(memory.physical[0]+128),&first_value,8);
        std::memcpy(reinterpret_cast<void*>(memory.physical[0]+256),&second_value,8);
        auto in=RandomState(rng,memory.stack+2*HostPage,fp);
        in.x[8]=memory.guest+128;in.pc=pointer_pc;
        auto expected=in;expected.x[8]=first_value;expected.pc=pointer_pc+4;
        const u32 original=*reinterpret_cast<const u32*>(restore_point);
        PatchImageWord(image,restore_point,0xd4200000);
        LoadState(cpu.ctx,in);auto hr=cpu.Run();
        const bool reached=hr==HrBreakpoint()&&cpu.ctx.pc==restore_point;
        PatchImageWord(image,restore_point,original);
        if(protected_restore) {
            memory.stack_alias_value=memory.Read64(cpu.ctx.sp);
            memory.stack_alias=cpu.ctx.sp;
            WindowProtect(memory.stack+HostPage,HostPage,PROT_NONE);
            // Resume at the actual LDR so hardware raises the synthetic restore fault.
        } else cpu.ctx.pc=wrong_retry?spilled_target:image.restart_pcs.at(restore_point);
        hr=cpu.Run();
        if(protected_restore)WindowProtect(memory.stack+HostPage,HostPage,PROT_READ|PROT_WRITE);
        memory.stack_alias=0;
        const auto diff=Diff(expected,StoreState(cpu.ctx,true));
        if (!wrong_retry && (!reached || hr!=HrBreakpoint() || diff.mismatches))
            std::fprintf(stderr,"pointer-resume protected=%d reached=%d hr=%llx stop=%llx sp=%llx diff=%s\n",
                protected_restore,reached,(unsigned long long)hr,(unsigned long long)cpu.ctx.pc,
                (unsigned long long)cpu.ctx.sp,diff.text.c_str());
        return reached&&hr==HrBreakpoint()&&diff.mismatches==0;
    };
    bool pointer_ok=restore_point&&spilled_target&&pointer_resume(false,false)&&pointer_resume(false,true);
    Expect("R7",pointer_ok,Class::Blocker,"Read that replaces its address: resume and hardware fault after the read without a second access","");
    bool retry_rejected=restore_point&&spilled_target&&!pointer_resume(true,false);
    Expect("R8",retry_rejected,Class::Blocker,"Negative control: wrongly replaying a finished read is rejected","");
    munmap(memory.table,1ULL<<30);
}
} // namespace
void RunReadThunkTests(){BeginStep("R","NCE reads and 4/16 KiB pages");Tests();EndStep();}
} // namespace ncev
