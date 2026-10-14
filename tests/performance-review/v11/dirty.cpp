#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>
#include <stdexcept>
using u32=uint32_t;using u8=uint8_t;using u64=uint64_t;
template<class F>struct Guard{F f;~Guard(){f();}};
struct ScopeExit{template<class F>auto operator+(F f){return Guard<F>{f};}};
#define SCOPE_EXIT const auto scope_exit_guard=ScopeExit{}+[&]()
struct Harness {
 struct Regs{enum class ShadowRamControl{Off,Track,TrackWithFilter,Replay};std::array<u32,4096> reg_array{};ShadowRamControl shadow_ram_control{};};
 struct DirtyState{using Flags=std::bitset<255>;using Tables=std::array<std::array<u8,4096>,2>;Flags flags;Tables tables{};};
 Regs regs,shadow_state;DirtyState dirty;std::vector<std::pair<u32,u32>> method_sink;
 void Baseline();void Probe();void ProcessDirtyRegisters(u32,u32);void ProcessCandidate(u32,u32);
 void ConsumeSinkOriginal(){Baseline();}
};
#define STRING2(x) #x
#define STRING(x) STRING2(x)
#include "dirty-base.inc"
#include STRING(DIRTY_INC)
volatile u32 sink;
int main(){std::mt19937 r(0x382913);for(unsigned trial=0;trial<40000;++trial){Harness a,b;for(unsigned i=0;i<4096;++i){a.regs.reg_array[i]=r()%16;a.shadow_state.reg_array[i]=r()%16;for(auto& table:a.dirty.tables)table[i]=r()%255;}for(unsigned i=0;i<255;++i)a.dirty.flags[i]=r()%2;a.shadow_state.shadow_ram_control=static_cast<Harness::Regs::ShadowRamControl>(trial%4);for(unsigned i=0,n=r()%512;i<n;++i)a.method_sink.emplace_back(r()%4096,r()%16);b=a;a.Baseline();b.Probe();if(a.regs.reg_array!=b.regs.reg_array||a.shadow_state.reg_array!=b.shadow_state.reg_array||a.dirty.flags!=b.dirty.flags||!b.method_sink.empty()){std::cerr<<"FAIL trial "<<trial<<'\n';return 1;}}std::cout<<"PASS40000 shadow track/filter/replay, repeated methods, dirty-bit preservation cases\n";
for(unsigned n:{1U,4U,16U,64U,256U})for(unsigned change:{0U,25U,100U})for(unsigned layout:{0U,1U,2U}){std::vector<double> timings[2];for(unsigned round=0;round<11;++round)for(unsigned order=0;order<2;++order){unsigned mode=(round+order)%2;Harness h;h.method_sink.reserve(n);for(unsigned i=0;i<4096;++i){const unsigned base=layout==2?64:0,mod=layout==0?64:layout==1?255:191;h.dirty.tables[0][i]=base+i%mod;h.dirty.tables[1][i]=i%3==0?base+i%mod:base+(i/2)%mod;}const auto start=std::chrono::steady_clock::now();for(unsigned it=0;it<40000;++it){for(unsigned j=0;j<n;++j)h.method_sink.emplace_back(j,(j%100<change)?it:0);if(mode)h.Probe();else h.Baseline();sink=h.regs.reg_array[n-1];}timings[mode].push_back(std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/40000);}std::cout<<"BENCH n="<<n<<" change="<<change<<" layout="<<layout;for(auto& t:timings){std::sort(t.begin(),t.end());std::cout<<' '<<t[5];}std::cout<<'\n';}return 0;}
