#include <array>
#include <bitset>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <cstdio>
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/puller.h"
using Maxwell3D=Tegra::Engines::Maxwell3D;
namespace Engines=Tegra::Engines;
constexpr u32 MacroRegistersStart=0xe00, non_puller_methods=0x40;
void Check(bool v,const char* s) { if(!v) throw std::runtime_error(s); }
using Regs=Maxwell3D::Regs;
struct Memory {
 std::map<GPUVAddr,Regs::ReportSemaphore::Compare> values;
 unsigned reads{};
 void ReadBlock(GPUVAddr a,void* p,size_t n) { ++reads; Check(n==sizeof(values[a]),"read size"); std::memcpy(p,&values[a],n); }
};
struct Draw {
 std::function<void()> on_flush=[]{};
 void DrawDeferred(){on_flush();}
 void ProcessMethodCall(u32,u32){}
};
struct Raster {
 std::function<bool()> accelerate=[]{return false;};
 unsigned calls{};
 bool AccelerateConditionalRendering(){++calls;return accelerate();}
 void WaitForIdle(){} void FragmentBarrier(){} void InvalidateGPUCache(){} void TiledCacheBarrier(){}
};
struct Macro { void ClearCode(u32){} void AddCode(u32,u32){} };
struct Upload { void ProcessExec(bool){} void ProcessData(u32,bool){} };
struct Fixture {
 using Regs=Maxwell3D::Regs;
 Regs regs{},shadow_state{};
 Memory memory_manager;
 Draw draw; Draw* draw_manager=&draw;
 Raster raster; Raster* rasterizer=&raster;
 Macro macro; Macro* macro_engine=&macro;
 Upload upload_state;
 bool execute_on=true;
 u32 executing_macro{};
 GPUVAddr current_dma_segment{};
 std::vector<std::pair<u32,u32>> method_sink;
 std::bitset<65535> execution_mask;
 Fixture(){ for(u32 i=0;i<Regs::NUM_REGS;++i) execution_mask[i]=IsMethodExecutable(i); }
 bool IsMethodExecutable(u32);
 u32 ProcessShadowRam(u32,u32);
 void ProcessQueryCondition();
 void ProcessMethodCall(u32,u32,u32,bool);
 void CallMethod(u32,u32,bool);
 void ProcessDirtyRegisters(u32 m,u32 a){regs.reg_array[m]=a;}
 void ConsumeSink(){for(auto [m,a]:method_sink)ProcessDirtyRegisters(m,ProcessShadowRam(m,a));method_sink.clear();}
 void ProcessMacro(u32,const u32*,u32,bool){}
 void ProcessMacroBind(u32){} void ProcessFirmwareCall4(){} void ProcessCBData(u32){} void ProcessCBBind(u32){}
 void ProcessQueryGet(){} void ProcessCounterReset(){} void ProcessSyncPoint(){}
};
struct Dma {
 struct {u32 method{},subchannel{},method_count=1; GPUVAddr dma_get{}; u32 dma_word_offset{};bool is_last_call=true;} dma_state;
 std::array<Fixture*,8> subchannels{};
 struct {void CallPullerMethod(Engines::Puller::MethodCall) const{}} puller;
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
 std::printf("engine transitions: %u checks PASS (production dispatch, CPU and mocked host paths)\n",cases);
}
