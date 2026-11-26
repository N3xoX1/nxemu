#include <span>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <cstdio>
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/puller.h"
#include "yuzu_video_core/dma_pusher.h"
using Maxwell3D=Tegra::Engines::Maxwell3D;
namespace Engines=Tegra::Engines;
using BufferMethods=Tegra::BufferMethods;
constexpr u32 MacroRegistersStart=0xe00, non_puller_methods=0x40;
enum { GEN_NONE = 0, GEN_STATE = 1 };
// Stream kinds: StreamHeader below ignores the kind and count, so any value compiles.
constexpr u32 StreamCall = 0;
void Check(bool v,const char* s) { if(!v) throw std::runtime_error(s); }
using Regs=Maxwell3D::Regs;
struct Memory {
 std::map<GPUVAddr,Regs::ReportSemaphore::Compare> values;
 unsigned reads{};
 std::map<GPUVAddr,DAddr> remapped_pages;
 void ReadBlock(GPUVAddr a,void* p,size_t n) {
  ++reads;
  auto it=values.upper_bound(a);
  if(it==values.begin()||a-std::prev(it)->first>=sizeof(Regs::ReportSemaphore::Compare)){std::memset(p,0,n);return;}
  --it; const size_t offset=a-it->first;
  Check(offset+n<=sizeof(it->second),"read past the condition");
  std::memcpy(p,reinterpret_cast<const u8*>(&it->second)+offset,n);
 }
 std::optional<DAddr> GpuToCpuAddress(GPUVAddr a) const {
  const auto page=remapped_pages.find(a&~GPUVAddr{0xfff});
  return page==remapped_pages.end()?a:page->second+(a&0xfff);
 }
};
struct { bool skip_conditional_render_wait=false; } videoSettings;
struct Draw {
 std::function<void()> on_flush=[]{};
 void DrawDeferred(){on_flush();}
 void ProcessMethodCall(u32,u32){}
};
struct Raster {
 std::function<bool()> accelerate=[]{return false;};
 unsigned calls{};
 bool AccelerateConditionalRendering(){++calls;return accelerate();}
 std::set<DAddr> pending;
 bool TryFlushQueryResults(DAddr a,u64 n){ auto it=pending.lower_bound(a); return it==pending.end()||*it>=a+n; }
 void WaitForIdle(){} void FragmentBarrier(){} void InvalidateGPUCache(){} void TiledCacheBarrier(){}
 void ReleaseDrawLocks(){}
};
struct Macro { void ClearCode(u32){} void AddCode(u32,u32){} };
struct Upload { void ProcessExec(bool){} void ProcessData(u32){} };
struct Fixture {
 using Regs=Maxwell3D::Regs;
 Regs regs{},shadow_state{},front_regs{};
 Memory memory_manager;
 Draw draw; Draw* draw_manager=&draw;
 Raster raster; Raster* rasterizer=&raster;
 Macro macro; Macro* macro_engine=&macro;
 Upload upload_state;
 bool execute_on=true;
 bool render_condition_skipped=false;
 u32 executing_macro{};
 GPUVAddr current_dma_segment{};
 std::vector<std::pair<u32,u32>> method_sink;
 std::bitset<65535> execution_mask;
 // Queued methods for the execute side: the test runs single-threaded, so publish right
 // after every call that has one (PublishWords=1) instead of batching like production.
 static constexpr u32 PublishWords = 1;
 std::vector<u32> stream;
 struct { std::array<u8, MacroRegistersStart> generation_kinds{}; } dirty;
 std::array<unsigned, 2> generations{};
 Fixture(){ for(u32 i=0;i<Regs::NUM_REGS;++i) execution_mask[i]=IsMethodExecutable(i); }
 bool IsMethodExecutable(u32);
 u32 ProcessShadowRam(u32,u32);
 void ProcessQueryCondition();
 bool ShouldExecuteClear();
 bool RenderConditionPasses(bool);
 std::optional<Regs::ReportSemaphore::Compare> ReadRenderCondition(bool,bool);
 void ProcessMethodCall(u32,u32,u32);
 void CallMethod(u32,u32,bool);
 void PushMethod(u32 m,u32 a){method_sink.emplace_back(m,a);}
 void ProcessDirtyRegisters(u32 m,u32 a){regs.reg_array[m]=a;}
 void ConsumeSink(){for(auto [m,a]:method_sink)ProcessDirtyRegisters(m,ProcessShadowRam(m,a));method_sink.clear();}
 void ProcessMacro(u32,const u32*,u32,bool){}
 void ProcessMacroBind(u32){} void ProcessFirmwareCall4(){} void ProcessCBData(u32){} void ProcessCBBind(u32){}
 void ProcessQueryGet(){} void ProcessCounterReset(){} void ProcessSyncPoint(){}
 bool IsThreaded() const { return true; }
 void FrontWrite(u32 method,u32 value){ front_regs.reg_array[method]=value; }
 void StreamHeader(u32,u32){}
 void FrontMethodCall(u32,u32,u32){}
 void Publish(){
  for(size_t i=0;i+3<=stream.size();i+=3){
   ProcessDirtyRegisters(stream[i],stream[i+1]);
   ProcessMethodCall(stream[i],stream[i+1],stream[i+2]);
  }
  stream.clear();
 }
};
struct Dma {
 struct {u32 method{},subchannel{},method_count=1; GPUVAddr dma_get{}; u32 dma_word_offset{};bool is_last_call=true;} dma_state;
 std::array<Fixture*,8> subchannels{};
 std::array<Engines::EngineTypes,8> subchannel_type{};
 struct {void CallPullerMethod(Engines::Puller::MethodCall) const{}} puller;
 void* maxwell3d_engine=nullptr;
 mutable std::vector<Engines::Puller::MethodCall> pending_puller;
 mutable std::array<std::vector<std::pair<u32,u32>>,8> front_sinks;
 mutable std::array<u32,non_puller_methods> front_puller_regs{};
 bool IsThreaded() const { return false; }
 void DrainMaxwell3D() const {}
 void UpdateUniformPrefetchAtAcquire(u32,u32) const {}
 bool IsPullerRegisterOnly(u32) const { return false; }
 bool IsPullerWait(u32,u32) const { return false; }
 struct BackWrites {};
 template<typename F> void PostBack(F&& f) const { f(); }
 template<typename F> void PostBack(F&& f, BackWrites) const { f(); }
 BackWrites PullerWrites(u32, std::span<const u32>) const { return {}; }
 BackWrites EngineWrites(u32, u32) const { return {}; }
 mutable u64 posted_dispatch{};
 void DispatchRan(u64) const {}
 void TrackFrontRegister(u32, u32, u32) const {}
 void ApplyFrontSink(u32) const {}
 std::vector<std::pair<u32,u32>>& SinkFor(u32 sc) const {
  if (IsThreaded() && subchannel_type[sc] != Engines::EngineTypes::Maxwell3D) { return front_sinks[sc]; }
  return subchannels[sc]->method_sink;
 }
 void CallMethod(u32) const;
};
#include "engine.inc"
int main(){
 unsigned cases=0;
 const auto test=[&](bool host){
  Fixture f; Dma d;d.subchannels[0]=&f;
  bool active=false;unsigned flushes=0;bool pending=false;bool old_cpu=false,old_host=false;
  f.draw.on_flush=[&]{++flushes;if(pending){Check(f.execute_on==old_cpu&&active==old_host,"deferred draw changed predicate");pending=false;}};
  f.raster.accelerate=[&]{Check(!pending,"predicate changed before deferred flush");active=host&&f.regs.render_enable_override==Regs::RenderEnable::Override::UseRenderEnable&&f.regs.render_enable.mode==Regs::RenderEnable::Mode::Conditional;return active;};
  auto write=[&](u32 m,u32 v){d.dma_state.method=m;d.CallMethod(v);};
  auto mode=[&](Regs::RenderEnable::Mode m){write(MAXWELL3D_REG_INDEX(render_enable.mode),u32(m));};
  auto over=[&](Regs::RenderEnable::Override o){write(MAXWELL3D_REG_INDEX(render_enable_override),u32(o));};
  const auto a=MAXWELL3D_REG_INDEX(render_enable.address_low),hi=MAXWELL3D_REG_INDEX(render_enable.address_high);
  mode(Regs::RenderEnable::Mode::True);
  auto calls=f.raster.calls;
  write(a,0x100);write(hi,0);Check(f.raster.calls==calls,"unconditional address write recomputed predicate");
  f.memory_manager.values[0x100]={1,1,0,0,1,1};
  f.memory_manager.values[0x200]={0,0,0,0,1,1};
  mode(Regs::RenderEnable::Mode::Conditional);Check(f.execute_on,"conditional true");
  pending=true;old_cpu=f.execute_on;old_host=active;
  over(Regs::RenderEnable::Override::NeverRender);Check(!f.execute_on&&!active,"NeverRender kept previous predicate");
  over(Regs::RenderEnable::Override::AlwaysRender);Check(f.execute_on&&!active,"AlwaysRender kept predicate");
  calls=f.raster.calls;write(a,0x200);Check(f.raster.calls==calls,"override address write recomputed predicate");
  over(Regs::RenderEnable::Override::UseRenderEnable);Check(f.execute_on==host,"restored override ignored current predicate");
  mode(Regs::RenderEnable::Mode::IfEqual);Check(!f.execute_on&&!active,"unequal values allowed");
  write(a,0x100);Check(f.execute_on,"address-only update ignored");
  mode(Regs::RenderEnable::Mode::IfNotEqual);Check(!f.execute_on,"not equal polarity");
  write(a,0x200);Check(f.execute_on,"address-only not equal update");
  f.memory_manager.values[0x100000200ULL]={1,1,0,0,1,1};
  write(hi,1);Check(!f.execute_on,"high address-only update ignored");
  mode(Regs::RenderEnable::Mode::False);Check(!f.execute_on&&!active,"False did not stop render");
  calls=f.raster.calls;write(a,0x300);Check(f.raster.calls==calls,"False address write recomputed predicate");
  Check(f.method_sink.empty(),"conditional register remained in method sink");
  Check(flushes==f.raster.calls,"deferred flush ordering/count");
  cases+=14;
 };
 test(false);test(true);
 {
  Fixture f; Dma d;d.subchannels[0]=&f;
  auto write=[&](u32 m,u32 v){d.dma_state.method=m;d.CallMethod(v);};
  auto mode=[&](Regs::RenderEnable::Mode m){write(MAXWELL3D_REG_INDEX(render_enable.mode),u32(m));};
  videoSettings.skip_conditional_render_wait=true;
  f.memory_manager.values[0x100]={0,0,0,0,1,1};
  write(MAXWELL3D_REG_INDEX(render_enable.address_low),0x100);
  f.raster.pending={0x108};
  auto reads=f.memory_manager.reads;
  mode(Regs::RenderEnable::Mode::Conditional);
  Check(!f.execute_on&&f.memory_manager.reads==reads+1,"pending result outside the compared words skipped a known condition");
  f.raster.pending={0x110};
  mode(Regs::RenderEnable::Mode::Conditional);
  Check(!f.execute_on,"conditional mode waited on the current operand");
  reads=f.memory_manager.reads;
  mode(Regs::RenderEnable::Mode::IfEqual);
  Check(f.execute_on&&f.memory_manager.reads==reads&&f.render_condition_skipped,"pending current operand was waited for");
  Check(!f.ShouldExecuteClear()&&f.memory_manager.reads==reads+2&&!f.execute_on,"clear did not wait for the skipped result");
  Check(!f.ShouldExecuteClear()&&f.memory_manager.reads==reads+2,"clear read a known result again");
  f.raster.pending={0x104};
  mode(Regs::RenderEnable::Mode::IfNotEqual);
  Check(f.execute_on&&f.render_condition_skipped,"pending initial operand was waited for");
  write(MAXWELL3D_REG_INDEX(render_enable_override),u32(Regs::RenderEnable::Override::NeverRender));
  Check(!f.render_condition_skipped&&!f.ShouldExecuteClear(),"override kept a skipped result");
  write(MAXWELL3D_REG_INDEX(render_enable_override),u32(Regs::RenderEnable::Override::UseRenderEnable));
  f.memory_manager.values[0x1ffc]={0,0,0,0,1,1};
  f.memory_manager.remapped_pages[0x2000]=0x9000;
  write(MAXWELL3D_REG_INDEX(render_enable.address_low),0x1ffc);
  f.raster.pending={0x2000};
  mode(Regs::RenderEnable::Mode::Conditional);
  Check(!f.execute_on&&!f.render_condition_skipped,"operand word on another page probed the adjacent device page");
  f.raster.pending={0x9000};
  mode(Regs::RenderEnable::Mode::IfEqual);
  Check(f.execute_on&&f.render_condition_skipped,"operand word on another page was not probed");
  videoSettings.skip_conditional_render_wait=false;
  cases+=9;
 }
 std::printf("engine transitions: %u checks PASS (production dispatch, CPU and mocked host paths)\n",cases);
}
