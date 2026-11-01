#include "yuzu_common/scope_exit.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/puller.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <stdexcept>
using Maxwell3D = Tegra::Engines::Maxwell3D;
namespace Engines = Tegra::Engines;
constexpr u32 MacroRegistersStart = 0xe00, non_puller_methods = 0x40;
void Check(bool v, const char *s) {
  if (!v)
    throw std::runtime_error(s);
}
using Regs = Maxwell3D::Regs;
struct Memory {
  std::map<GPUVAddr, Regs::ReportSemaphore::Compare> values;
  unsigned reads{};
  void ReadBlock(GPUVAddr a, void *p, size_t n) {
    ++reads;
    Check(n == sizeof(values[a]), "read size");
    std::memcpy(p, &values[a], n);
  }
};
struct Draw {
  std::function<void()> on_flush = [] {};
  void DrawDeferred() { on_flush(); }
  std::function<void(u32, u32)> on_method = [](u32, u32) {};
  void ProcessMethodCall(u32 m, u32 a) { on_method(m, a); }
};
struct Raster {
  std::function<bool()> accelerate = [] { return false; };
  unsigned calls{};
  bool AccelerateConditionalRendering() {
    ++calls;
    return accelerate();
  }
  void WaitForIdle() {}
  void FragmentBarrier() {}
  void InvalidateGPUCache() {}
  void TiledCacheBarrier() {}
};
struct Macro {
  void ClearCode(u32) {}
  void AddCode(u32, u32) {}
};
struct Upload {
  void ProcessExec(bool) {}
  void ProcessData(u32, bool) {}
};
struct Fixture : Engines::EngineInterface {
  using DirtyState = Maxwell3D::DirtyState;
  DirtyState dirty;
  std::vector<std::array<u32, 4>> effects;
  std::vector<std::array<u32, Regs::NUM_REGS>> observed;
  using Regs = Maxwell3D::Regs;
  Regs regs{}, shadow_state{};
  Memory memory_manager;
  Draw draw;
  Draw *draw_manager = &draw;
  Raster raster;
  Raster *rasterizer = &raster;
  Macro macro;
  Macro *macro_engine = &macro;
  Upload upload_state;
  bool execute_on = true;
  u32 executing_macro{};

  Fixture() {
    draw.on_method = [this](u32 m, u32 a) {
      effects.push_back(
          {m, a, regs.reg_array[0x700], u32(current_dma_segment)});
      observed.push_back(regs.reg_array);
    };
    method_sink.reserve(256);
    for (u32 i = 0; i < execution_mask.size(); ++i)
      execution_mask[i] = IsMethodExecutable(i);
  }
  bool IsMethodExecutable(u32);
  u32 ProcessShadowRam(u32, u32);
  void ProcessQueryCondition() { effects.push_back({1, 2, 3, 4}); }
  void ProcessMethodCall(u32, u32, u32, bool);
  void CallMethod(u32, u32, bool);
  void ProcessDirtyRegisters(u32, u32);
  void PushMethod(u32, u32) override;
  void PushMethodRun(u32, std::span<const Tegra::CommandHeader>) override;
  void ConsumeSinkImpl() override;
  void CallMultiMethod(u32 m, const u32 *a, u32 n, u32 pending) override {
    for (u32 i = 0; i < n; ++i)
      CallMethod(m, a[i], pending - i <= 1);
  }
  void ProcessMacro(u32 m, const u32 *a, u32 n, bool last) {
    effects.push_back({m, *a, n, u32(last)});
  }
  void ProcessMacroBind(u32) {}
  void ProcessFirmwareCall4() {}
  void ProcessCBData(u32) {}
  void ProcessCBBind(u32) {}
  void ProcessQueryGet() {}
  void ProcessCounterReset() {}
  void ProcessSyncPoint() {}
};

using Tegra::CommandHeader;
using Tegra::SubmissionMode;
struct Dma {
  struct State {
    u32 method{}, subchannel{}, method_count{};
    GPUVAddr dma_get = 0x1000;
    u64 dma_word_offset{};
    bool non_incrementing{}, is_last_call = true;
  } dma_state;
  bool dma_increment_once{};
  std::array<Engines::EngineInterface *, 8> subchannels{};
  std::array<Engines::EngineTypes, 8> subchannel_type;
  Dma() { subchannel_type.fill(Engines::EngineTypes::Maxwell3D); }
  struct Puller {
    void CallPullerMethod(Engines::Puller::MethodCall) const {}
    void CallMultiMethod(u32, u32, const u32 *, u32, u32) const {}
  } puller;
  void ProcessCommands(std::span<const CommandHeader>);
  void SetState(const CommandHeader &);
  void CallMethod(u32) const;
  void CallMultiMethod(const u32 *, u32) const;
};
struct QueuedFixture : Fixture {
  void PushMethod(u32 m, u32 a) override { EngineInterface::PushMethod(m, a); }
  void PushMethodRun(u32 m, std::span<const CommandHeader> a) override {
    EngineInterface::PushMethodRun(m, a);
  }
};
struct BaselineDma : Dma {
  void ProcessCommands(std::span<const CommandHeader>);
  void SetState(const CommandHeader &);
  void CallMethod(u32) const;
  void CallMultiMethod(const u32 *, u32) const;
};
#include "baseline.inc"
#include "dma.inc"
#include "engine.inc"

CommandHeader Header(u32 m, u32 n, SubmissionMode mode, u32 channel = 0) {
  CommandHeader h{};
  h.method.Assign(m);
  h.arg_count.Assign(n);
  h.mode.Assign(mode);
  h.subchannel.Assign(channel);
  return h;
}
void Append(std::vector<CommandHeader> &c, u32 m, std::initializer_list<u32> a,
            SubmissionMode mode = SubmissionMode::Increasing) {
  c.push_back(Header(m, u32(a.size()), mode));
  for (u32 v : a)
    c.push_back(CommandHeader{v});
}
void Compare(Fixture &a, Fixture &b) {
  a.ConsumeSink();
  b.ConsumeSink();
  Check(a.regs.reg_array == b.regs.reg_array, "register mismatch");
  Check(a.shadow_state.reg_array == b.shadow_state.reg_array,
        "shadow mismatch");
  Check(a.dirty.flags == b.dirty.flags, "dirty mismatch");
  Check(a.dirty.shader_profile_changes == b.dirty.shader_profile_changes,
        "profile invalidation mismatch");
  Check(a.observed == b.observed, "register state at executable mismatch");
  Check(a.effects == b.effects, "executable effects/order/address mismatch");
}
void Tests() {
  size_t cases = 0;
  for (u32 control = 0; control < 4; ++control) {
    for (size_t split = 1; split <= 24; ++split) {
      Fixture a, b;
      Dma d;
      BaselineDma ref;
      d.subchannels.fill(&a);
      ref.subchannels.fill(&b);
      for (size_t i = 0; i < Regs::NUM_REGS; ++i) {
        a.shadow_state.reg_array[i] = b.shadow_state.reg_array[i] = u32(i * 3);
        a.dirty.tables[0][i] = b.dirty.tables[0][i] = u8(i % 254);
        a.dirty.tables[1][i] = b.dirty.tables[1][i] = u8((i * 7) % 254);
        a.dirty.shader_profile_registers[i] =
            b.dirty.shader_profile_registers[i] = u8(i % 8);
      }
      std::vector<CommandHeader> c;
      Append(c, MAXWELL3D_REG_INDEX(shadow_ram_control), {control});
      Append(c, 0x700, {1, 2, 3, 4, 5, 6});
      Append(c, 0x700, {1, 2, 3, 4, 5, 6});
      Append(c, 0x710, {7, 8, 9, 10}, SubmissionMode::IncreaseOnce);
      Append(c, MAXWELL3D_REG_INDEX(topology_override), {1});
      Append(c, 0x720, {3, 4, 5}, SubmissionMode::NonIncreasing);
      c.push_back(Header(0x730, 33, SubmissionMode::Inline));
      Append(c, MAXWELL3D_REG_INDEX(draw.begin), {3});
      Append(c, MAXWELL3D_REG_INDEX(draw.end), {0});
      Append(c, MAXWELL3D_REG_INDEX(draw.end), {0});
      Append(c, 0xe00, {11, 12, 13}, SubmissionMode::IncreaseOnce);
      Append(c, MAXWELL3D_REG_INDEX(shadow_ram_control), {2});
      Append(c, 0x700, {99, 98, 97}, SubmissionMode::Increasing);
      Append(c, MAXWELL3D_REG_INDEX(topology_override), {2});
      for (size_t i = 0; i < c.size(); i += split) {
        auto part = std::span(c).subspan(i, std::min(split, c.size() - i));
        d.ProcessCommands(part);
        ref.ProcessCommands(part);
      }
      Compare(a, b);
      Check(d.dma_state.method == ref.dma_state.method &&
                d.dma_state.method_count == ref.dma_state.method_count &&
                d.dma_state.non_incrementing == ref.dma_state.non_incrementing,
            "DMA continuation mismatch");
      ++cases;
    }
  }
  // Other engines retain deferred register delivery through the default
  // implementation.
  struct Other : Engines::EngineInterface {
    std::vector<std::pair<u32, u32>> writes;
    void CallMethod(u32 m, u32 a, bool) override { writes.emplace_back(m, a); }
    void CallMultiMethod(u32, const u32 *, u32, u32) override {}
  } other;
  Dma d;
  d.subchannel_type.fill(Engines::EngineTypes::KeplerCompute);
  d.subchannels.fill(&other);
  std::vector<CommandHeader> c;
  Append(c, 0x700, {1, 2, 3});
  d.ProcessCommands(c);
  Check(other.writes.empty(), "other engine delivery changed");
  other.ConsumeSink();
  Check(other.writes.size() == 3, "other engine lost writes");
  // Count dispatches without instrumenting the timed production loops.
  struct Counting : Fixture {
    unsigned runs{}, drains{}, calls{};
    void PushMethodRun(u32 m, std::span<const CommandHeader> a) override {
      ++runs;
      Fixture::PushMethodRun(m, a);
    }
    void ConsumeSinkImpl() override {
      ++drains;
      Fixture::ConsumeSinkImpl();
    }
    void CallMethod(u32 m, u32 a, bool last) override {
      ++calls;
      Fixture::CallMethod(m, a, last);
    }
  } counted, original;
  Dma measured;
  BaselineDma old;
  measured.subchannels.fill(&counted);
  old.subchannels.fill(&original);
  std::vector<CommandHeader> run{Header(0x700, 64, SubmissionMode::Increasing)};
  for (u32 i = 0; i < 64; ++i)
    run.push_back(CommandHeader{i + 1});
  measured.ProcessCommands(run);
  old.ProcessCommands(run);
  Check(original.method_sink.size() == 64 && counted.method_sink.empty(),
        "queued writes were not eliminated");
  run.clear();
  Append(run, MAXWELL3D_REG_INDEX(topology_override), {1});
  measured.ProcessCommands(run);
  old.ProcessCommands(run);
  Check(counted.runs == 1 && counted.drains == 0 && original.drains == 1 &&
            counted.calls == 1 && original.calls == 1,
        "virtual dispatch count changed unexpectedly");
  // Deterministic random register runs, executable boundaries and shadow
  // transitions.
  std::mt19937 rng(0x4e58454d);
  for (unsigned trial = 0; trial < 256; ++trial) {
    Fixture a, b;
    Dma d;
    BaselineDma ref;
    d.subchannels.fill(&a);
    ref.subchannels.fill(&b);
    std::vector<CommandHeader> c;
    for (unsigned block = 0; block < 32; ++block) {
      Append(c, MAXWELL3D_REG_INDEX(shadow_ram_control), {rng() % 4});
      const u32 first = 0x700 + rng() % 32, count = 1 + rng() % 32;
      c.push_back(
          Header(first, count,
                 static_cast<SubmissionMode>(std::array{1u, 3u, 5u}[rng() % 3]),
                 rng() % 8));
      for (u32 i = 0; i < count; ++i)
        c.push_back(CommandHeader{rng()});
      Append(c, MAXWELL3D_REG_INDEX(topology_override), {rng() % 4});
    }
    for (size_t i = 0; i < c.size();) {
      size_t n = std::min<size_t>(1 + rng() % 17, c.size() - i);
      auto part = std::span(c).subspan(i, n);
      d.ProcessCommands(part);
      ref.ProcessCommands(part);
      i += n;
    }
    Compare(a, b);
    ++cases;
  }
  std::printf("%zu command-stream differential cases + default engine passed\n",
              cases);
}
template <class Pusher, class Engine = Fixture>
double Bench(size_t run, bool changed, u32 control) {
  Engine f;
  Pusher d;
  if constexpr (std::is_same_v<Engine, QueuedFixture>)
    d.subchannel_type.fill(Engines::EngineTypes::KeplerCompute);
  d.subchannels.fill(&f);
  f.shadow_state.shadow_ram_control =
      static_cast<Regs::ShadowRamControl>(control);
  std::vector<CommandHeader> c;
  c.push_back(Header(0x700, u32(run), SubmissionMode::Increasing));
  for (size_t i = 0; i < run; ++i)
    c.push_back(CommandHeader{u32(i + 1)});
  constexpr size_t loops = 200000;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < loops; ++i) {
    if (changed)
      c[1].argument = u32(i);
    d.ProcessCommands(c);
    f.ConsumeSink();
  }
  const auto end = std::chrono::steady_clock::now();
  Check(f.regs.reg_array[0x700] ==
            (control == 3 ? 0 : (changed ? u32(loops - 1) : 1)),
        "benchmark result");
  return std::chrono::duration<double, std::nano>(end - start).count() / loops;
}

// Evaluate the fork's NullEntry filtering independently from DMA batching.
struct FilteredDirty : Fixture {
  void ProcessDirtyRegisters(u32 method, u32 argument) {
    if (regs.reg_array[method] == argument)
      return;
    regs.reg_array[method] = argument;
    dirty.MarkShaderProfileRegister(method);
    for (const auto &table : dirty.tables)
      if (const u8 flag = table[method]; flag != 0)
        dirty.flags[flag] = true;
  }
};
#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif
template <class Engine> TEST_NOINLINE void DirtyWrite(Engine &f, u32 argument) {
  f.ProcessDirtyRegisters(0x700, argument);
}
template <class Engine> double DirtyBench(u8 first, u8 second) {
  Engine f;
  f.dirty.tables[0][0x700] = first;
  f.dirty.tables[1][0x700] = second;
  const auto start = std::chrono::steady_clock::now();
  for (u32 i = 0; i < 2000000; ++i)
    DirtyWrite(f, i);
  const auto end = std::chrono::steady_clock::now();
  Check(f.regs.reg_array[0x700] == 1999999, "dirty benchmark result");
  // Keep flags/profile observable so the compiler cannot drop the hot writes.
  std::fprintf(stderr, "flags=%zu profile=%llu ", f.dirty.flags.count(),
               static_cast<unsigned long long>(f.dirty.shader_profile_changes));
  return std::chrono::duration<double, std::nano>(end - start).count() /
         2000000;
}
int main(int argc, char **) {
  Tests();
  if (argc < 2)
    return 0;
  std::puts("run,changed,shadow,sample,baseline_ns,candidate_ns");
  for (auto pair : {std::pair<u8, u8>{0, 0}, {1, 0}, {1, 2}, {80, 0}, {80, 81}})
    for (int i = 0; i < 9; ++i) {
      double b = DirtyBench<Fixture>(pair.first, pair.second),
             a = DirtyBench<FilteredDirty>(pair.first, pair.second);
      std::fprintf(stderr, "dirty %u %u %d %.3f %.3f\n", pair.first,
                   pair.second, i, b, a);
    }
  for (size_t run : {1u, 4u, 16u, 64u}) {
    double before = Bench<BaselineDma, QueuedFixture>(run, false, 2),
           after = Bench<Dma, QueuedFixture>(run, false, 2);
    std::fprintf(stderr, "queued %zu %.3f %.3f\n", run, before, after);
  }
  for (u32 shadow : {0u, 1u, 2u, 3u})
    for (size_t run : {1u, 4u, 16u, 64u})
      for (bool changed : {false, true})
        for (int sample = 0; sample < 9; ++sample) {
          double before, after;
          if (sample % 2) {
            after = Bench<Dma>(run, changed, shadow);
            before = Bench<BaselineDma>(run, changed, shadow);
          } else {
            before = Bench<BaselineDma>(run, changed, shadow);
            after = Bench<Dma>(run, changed, shadow);
          }
          std::printf("%zu,%d,%u,%d,%.3f,%.3f\n", run, changed, shadow, sample,
                      before, after);
        }
}
