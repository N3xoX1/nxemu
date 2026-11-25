#include "yuzu_common/scope_exit.h"
#include "yuzu_video_core/buffer_cache/uniform_stream_reuse.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_common/alignment.h"
#include "yuzu_video_core/engines/engine_upload.h"
#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/maxwell_dma.h"
#include "yuzu_video_core/engines/puller.h"
#include "yuzu_video_core/gpu_write_history.h"
#include "yuzu_video_core/macro/macro.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
using Maxwell3D = Tegra::Engines::Maxwell3D;
namespace Engines = Tegra::Engines;
constexpr u32 MacroRegistersStart = 0xe00, non_puller_methods = 0x40;
void Check(bool v, const char *s) {
  if (!v)
    throw std::runtime_error(s);
}
using Regs = Maxwell3D::Regs;
using Tegra::CachedMacro;
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
  void ApplyUniformPrefetch(size_t, u32, const VideoCommon::UniformPrefetch &) {}
  void ResetUniformPrefetch(u64) {}
  void ReleaseDrawLocks() {}
};
struct Macro {
  void ClearCode(u32) {}
  void AddCode(u32, u32) {}
};
struct Upload {
  void ProcessExec(bool) {}
  void ProcessData(u32) {}
  void ProcessData(const u32 *, size_t) {}
};
#include "stream.inc"
struct Fixture : Engines::EngineInterface {
  using DirtyState = Maxwell3D::DirtyState;
  DirtyState dirty;
  using GenerationKind = Maxwell3D::GenerationKind;
  static constexpr GenerationKind GEN_STATE = Maxwell3D::GEN_STATE;
  static constexpr GenerationKind GEN_NONE = Maxwell3D::GEN_NONE;
  static constexpr GenerationKind GEN_BUFFERS = Maxwell3D::GEN_BUFFERS;
  struct {
    std::array<u32, Regs::MaxShaderStage> rebound_slots{};
  } buffer_changes;
  std::array<u64, 3> generations{};
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
  // Decode side state; without a decode thread the stream runs on DrainStream
  using HLECall = Maxwell3D::HLECall;
  static constexpr size_t PublishWords = 4096;
  Regs front_regs{};
  std::vector<u32> stream;
  size_t stream_run_pos{};
  u32 stream_run_next{};
  bool stream_run_open{};
  bool stream_writes_memory{};
  std::vector<std::pair<GPUVAddr, GPUVAddr>> stream_write_ranges;
  bool stream_writes_anywhere{};
  struct LongQueryReport {
    u64 value, timestamp;
  };
  void FrontMemoryWrite(GPUVAddr address, u64 size) {
    stream_write_ranges.emplace_back(address, address + size);
  }
  std::vector<HLECall> hle_calls;
  HLECall *back_hle{};
  size_t hle_count{};
  std::vector<std::pair<GPUVAddr, size_t>> macro_segments;
  std::vector<GPUVAddr> macro_addresses;
  bool current_macro_dirty{}, current_macro_payload{};
  unsigned refreshes{}, unflushed_refreshes{};
  struct Back {
    template <typename F> void RunExclusive(F&& f) { f(); }
  } back;
  Back* back_queue = &back;
  void FrontHLEMacro(CachedMacro*, const std::vector<u32>&, u32);

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
  void ProcessMethodCall(u32, u32, u32);
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
  void FrontBindUniform(size_t) {}
  void ProcessQueryGet() {}
  void ProcessCounterReset() {}
  void ProcessSyncPoint() {}
  void ProcessCBMultiData(const u32 *, u32) {}
  bool AnySegmentDirty(const std::vector<std::pair<GPUVAddr, size_t>> &) const {
    return false;
  }
  void RefreshParametersImpl(const std::vector<std::pair<GPUVAddr, size_t>> &,
                             std::vector<u32> &, bool flush = true) {
    ++(flush ? refreshes : unflushed_refreshes);
  }
  void FrontWrite(u32, u32);
  void StreamHeader(StreamKind, u32);
  void FrontMethodCall(u32, u32, u32);
  void RunWords(const u32 *, size_t, std::vector<HLECall> &);
  bool IsThreaded() const { return false; }
  void DrainStream() {
    stream_run_open = false;
    RunWords(stream.data(), stream.size(), hle_calls);
    stream.clear();
    stream_writes_memory = false;
    stream_write_ranges.clear();
    stream_writes_anywhere = false;
    hle_count = 0;
  }
  void Publish(bool = true) { DrainStream(); }
  void SyncBack() { DrainStream(); }
};

using Tegra::BufferMethods;
using Tegra::CommandHeader;
using Tegra::SubmissionMode;
namespace Settings {
bool strict_compute_ordering{};
bool IsStrictComputeOrderingEnabled() { return strict_compute_ordering; }
} // namespace Settings

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
  mutable std::array<std::vector<std::pair<u32, u32>>, 8> front_sinks;
  mutable std::vector<Engines::Puller::MethodCall> pending_puller;
  mutable std::array<u32, non_puller_methods> front_puller_regs{};
  mutable std::vector<std::string> *events{};
  mutable std::vector<std::pair<u32, u32>> acquire_checks;
  bool acquires_met{};
  bool IsAcquireMet(u32 m, u32 a) const {
    acquire_checks.push_back({m, a});
    return acquires_met;
  }
  mutable bool unmet_acquire_seen{};
  mutable bool queued_dispatch{};
  mutable std::atomic<bool> strict_dispatch_wait{};
  mutable u64 launched_dispatches{};
  mutable u64 posted_dispatch{};
  mutable std::vector<u64> ran_dispatches;
  void DispatchRan(u64 id) const { ran_dispatches.push_back(id); }
  Engines::EngineInterface *maxwell3d_engine{};
  struct Gpu {
    struct Queue {
      std::vector<std::string> **events;
      bool idle = true;
      void WaitIdle() { (*events)->push_back("wait"); }
      bool IsIdle() const { return idle; }
    } queue;
    Queue &BackQueue() { return queue; }
  };
  mutable Gpu gpu{{&events}};
  void UpdateUniformPrefetchAtAcquire(u32, u32) const;
  void ResumeUniformPrefetch() const;
  bool threaded{};
  bool IsThreaded() const { return threaded; }
  void DrainMaxwell3D() const {}
  struct BackWrites {
    enum class Kind { Anywhere, Range, Nothing };
    Kind kind{Kind::Anywhere};
    GPUVAddr address{};
    u64 size{};
    GPUVAddr extra_address{};
    u64 extra_size{};
  };
  mutable std::vector<BackWrites> posted;
  /// Keeps the work queued, as the GPU thread would, until the test runs it
  bool defer{};
  mutable std::vector<std::function<void()>> queued;
  void PostBack(std::function<void()> &&work) const { PostBack(std::move(work), BackWrites{}); }
  void PostBack(std::function<void()> &&work, BackWrites writes) const {
    posted.push_back(writes);
    if (defer) {
      queued.push_back(std::move(work));
      return;
    }
    work();
  }
  void RunQueued() {
    for (auto &work : queued)
      work();
    queued.clear();
  }
  BackWrites PullerWrites(u32, std::span<const u32>) const;
  BackWrites EngineWrites(u32, u32) const;
  BackWrites UploadWrites(u32) const;
  BackWrites CopyWrites() const;
  BackWrites BlitWrites() const;
  static u64 BlockLinearSize(u64, u32, u32, u32, u32);
  void TrackFrontRegister(u32, u32, u32) const;
  void ApplyFrontSink(u32) const;
  mutable std::array<std::array<u32, 13>, 2> front_upload{};
  mutable std::array<u32, 0x800> front_copy{};
  mutable std::array<u32, 0x258> front_blit{};
  std::vector<std::pair<u32, u32>> &SinkFor(u32) const;
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
  a.DrainStream();
  b.DrainStream();
  Check(a.regs.reg_array == b.regs.reg_array, "register mismatch");
  Check(a.shadow_state.reg_array == b.shadow_state.reg_array,
        "shadow mismatch");
  Check(a.dirty.flags == b.dirty.flags, "dirty mismatch");
  Check(a.dirty.shader_profile_changes == b.dirty.shader_profile_changes,
        "profile invalidation mismatch");
  Check(a.observed == b.observed, "register state at executable mismatch");
  Check(a.effects == b.effects, "executable effects/order/address mismatch");
}
// A backend can reject indirect drawing even for a safe topology. The real
// stream dispatcher must retain the fallback's register effects on both sides.
void MacroStreamTests() {
  struct Fallback : CachedMacro {
    Fixture& f;
    explicit Fallback(Fixture& f_) : f(f_) {}
    void Execute(const std::vector<u32>&, u32) override {
      f.regs.reg_array[0x700] = 42;
    }
    std::span<const u32> FrontEffectRegisters() const override {
      static constexpr std::array<u32, 1> registers{0x700};
      return registers;
    }
  };
  Fixture f;
  Fallback fallback(f);
  f.FrontHLEMacro(&fallback, {}, 0);
  Check(f.regs.reg_array[0x700] == 42 && f.front_regs.reg_array[0x700] == 42,
        "fallback effects must propagate to decode, not be overwritten");
  f.FrontWrite(0x700, 0);
  f.DrainStream();
  Check(f.regs.reg_array[0x700] == 0, "write after fallback was incorrectly elided");

  // Dirty indirect arguments stay on the GPU unless Execute needs a fallback, the guest copy is
  // read again without a flush
  f.hle_calls.resize(1);
  auto& call = f.hle_calls[0];
  call.program = &fallback;
  call.recheck = true;
  call.dirty = true;
  const std::array<u32, 1> words{StreamHLE};
  f.RunWords(words.data(), words.size(), f.hle_calls);
  Check(f.refreshes == 0, "indirect execution forced a GPU argument readback");
  Check(f.unflushed_refreshes == 1, "GPU-written parameters kept the decode thread copy");
  call.dirty = false;
  f.RunWords(words.data(), words.size(), f.hle_calls);
  Check(f.refreshes == 1, "CPU-written parameters were not refreshed");

  // A uniform prefetch rebinds its slot, a prefetch reset rebinds everything
  {
    const u32 stage = 2, index = 5;
    const std::array<u32, 8> prefetch{StreamPrefetch | ((stage | (index << 3)) << 8), 0x1000, 0, 0x100,
                                      0, StreamPrefetchReset, 0, 0};
    const auto before = f.generations;
    f.RunWords(prefetch.data(), 5, f.hle_calls);
    Check(f.generations[Fixture::GEN_BUFFERS] == before[Fixture::GEN_BUFFERS] + 1 &&
              (f.buffer_changes.rebound_slots[stage] & (1U << index)) != 0,
          "a uniform prefetch kept the reused binding");
    f.RunWords(prefetch.data() + 5, 3, f.hle_calls);
    Check(f.generations[Fixture::GEN_STATE] == before[Fixture::GEN_STATE] + 1,
          "a prefetch reset kept the reused bindings");
  }

  // Macros lowered to register writes keep the call when the GPU may write the parameters
  struct Lowered : CachedMacro {
    Fixture& f;
    explicit Lowered(Fixture& f_) : f(f_) {}
    void Execute(const std::vector<u32>& p, u32) override {
      f.regs.reg_array[0x701] = p[0];
    }
    bool LowerToWrites(const std::vector<u32>& p) override {
      f.FrontWrite(0x701, p[0]);
      return true;
    }
  };
  Fixture g;
  Lowered lowered(g);
  g.FrontHLEMacro(&lowered, {5}, 0);
  Check(g.hle_count == 0, "clean parameters did not lower to register writes");
  g.DrainStream();
  Check(g.regs.reg_array[0x701] == 5, "lowered register write was lost");
  g.current_macro_payload = true;
  g.FrontHLEMacro(&lowered, {6}, 0);
  Check(g.hle_count == 1, "payload parameters were lowered before the GPU wrote them");
  g.DrainStream();
  Check(g.regs.reg_array[0x701] == 6, "kept macro call did not run");
}

struct PrefetchEngine : Engines::EngineInterface {
  std::vector<std::string> &events;
  explicit PrefetchEngine(std::vector<std::string> &events_) : events(events_) {}
  void CallMethod(u32, u32, bool) override {}
  void CallMultiMethod(u32, const u32 *, u32, u32) override {}
  bool prefetching = true;
  void StartUniformPrefetch() override {
    events.push_back("start");
    prefetching = true;
  }
  void RefreshUniformPrefetch() override {
    if (prefetching) {
      events.push_back("refresh");
    }
  }
  bool StopUniformPrefetch() override {
    events.push_back("stop");
    return std::exchange(prefetching, false);
  }
};

void UniformPrefetchStopTests() {
  std::vector<std::string> events;
  PrefetchEngine engine(events);
  Dma d;
  d.threaded = true;
  d.events = &events;
  d.maxwell3d_engine = &engine;
  const u32 high = u32(BufferMethods::SemaphoreAddressHigh);
  const u32 operation = u32(BufferMethods::SemaphoreOperation);
  std::vector<CommandHeader> c;
  Append(c, high, {0x1, 0x2000, 7, 0x2});
  Append(c, operation, {0x1, 0x8}, SubmissionMode::NonIncreasing);
  d.ProcessCommands(c);
  const std::vector<std::pair<u32, u32>> acquires{{operation, 0x1}, {operation, 0x8}};
  Check(d.acquire_checks == acquires, "an acquire skipped the check, or a release was checked");
  Check(d.front_puller_regs[high + 2] == 7 && d.front_puller_regs[operation] == 0x8,
        "the decode side lost a puller register");
  Check(events == std::vector<std::string>{"stop", "stop"}, "an unmet acquire kept prefetching");

  events.clear();
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"wait", "start"},
        "prefetch resumed before the GPU thread ran the acquire");
  events.clear();
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"start"}, "a met command list waited again");

  d.acquires_met = true;
  Append(c, operation, {0x4});
  events.clear();
  d.ProcessCommands(std::span(c).last(2));
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"refresh", "start"},
        "a met acquire stopped prefetch or kept the earlier copies");

  d.gpu.queue.idle = false;
  events.clear();
  d.ProcessCommands(std::span(c).last(2));
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"stop", "wait", "start"},
        "a semaphore met ahead of queued work kept prefetching");
  const u32 syncpoint = u32(BufferMethods::SyncpointOperation);
  Append(c, syncpoint, {0x0});
  events.clear();
  d.ProcessCommands(std::span(c).last(2));
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"refresh", "start"},
        "a met syncpoint stopped prefetch or kept the earlier copies");
  d.gpu.queue.idle = true;

  d.acquires_met = false;
  engine.prefetching = false;
  d.ProcessCommands(std::span(c).last(2));
  events.clear();
  d.ResumeUniformPrefetch();
  Check(events == std::vector<std::string>{"start"}, "a channel that wasn't prefetching waited");
}

struct UploadEngine : Engines::EngineInterface {
  std::vector<u32> calls;
  void CallMethod(u32 method, u32, bool) override { calls.push_back(method); }
  void CallMultiMethod(u32 method, const u32 *, u32, u32) override { calls.push_back(method); }
};

void QueuedWritesTests() {
  using Kind = Dma::BackWrites::Kind;
  const auto is = [](const Dma::BackWrites &w, Kind kind, GPUVAddr address = 0, u64 size = 0) {
    return w.kind == kind && (kind != Kind::Range || (w.address == address && w.size == size));
  };
  Dma d;
  d.threaded = true;
  std::vector<CommandHeader> c;
  Append(c, u32(BufferMethods::SemaphoreAddressHigh), {0x1, 0x2000});
  Append(c, u32(BufferMethods::SemaphoreRelease), {5});
  d.ProcessCommands(c);
  Check(d.posted.size() == 1 && is(d.posted[0], Kind::Range, 0x100002000, 4),
        "a semaphore release did not declare its payload");
  struct Operation {
    std::vector<u32> arguments;
    Kind kind;
    u64 size;
  };
  const std::array<Operation, 3> operations{{
      {{0x2}, Kind::Range, 16}, {{0x1}, Kind::Anywhere, 0}, {{0x2, 0x1}, Kind::Anywhere, 0}}};
  for (const Operation &operation : operations) {
    c.clear();
    d.posted.clear();
    c.push_back(Header(u32(BufferMethods::SemaphoreOperation), u32(operation.arguments.size()),
                       SubmissionMode::NonIncreasing));
    for (u32 v : operation.arguments)
      c.push_back(CommandHeader{v});
    d.ProcessCommands(c);
    Check(d.posted.size() == 1 && is(d.posted[0], operation.kind, 0x100002000, operation.size),
          "a semaphore operation declared the wrong writes");
  }
  for (const auto &[method, argument] :
       {std::pair{BufferMethods::RefCnt, 0U}, std::pair{BufferMethods::WaitForIdle, 0U},
        std::pair{BufferMethods::MemOpB, 0U}, std::pair{BufferMethods::SyncpointOperation, 1U}}) {
    c.clear();
    d.posted.clear();
    Append(c, u32(method), {argument});
    d.ProcessCommands(c);
    Check(d.posted.size() == 1 && is(d.posted[0], Kind::Nothing),
          "a puller method that writes no memory made decode wait");
  }
  // The producer that releases an acquire may write the next entries first
  for (const auto &[method, argument] : {std::pair{BufferMethods::SemaphoreAcquire, 5U},
                                         std::pair{BufferMethods::SyncpointOperation, 0U}}) {
    c.clear();
    d.posted.clear();
    Append(c, u32(method), {argument});
    d.ProcessCommands(c);
    Check(d.posted.size() == 1 && is(d.posted[0], Kind::Anywhere),
          "decode fetched past an acquire the GPU thread had not run");
  }
  c.clear();
  d.posted.clear();
  Append(c, u32(BufferMethods::CRCCheck), {0});
  d.ProcessCommands(c);
  Check(d.posted.size() == 1 && is(d.posted[0], Kind::Anywhere),
        "an unknown puller method stopped waiting");

  for (const auto type : {Engines::EngineTypes::KeplerCompute, Engines::EngineTypes::KeplerMemory}) {
    UploadEngine engine;
    engine.execution_mask[0x6C] = engine.execution_mask[0x6D] = true;
    if (type == Engines::EngineTypes::KeplerCompute) {
      engine.execution_mask[0xAF] = true;
    }
    Dma e;
    e.threaded = true;
    e.subchannels[1] = &engine;
    e.subchannel_type[1] = type;
    const auto on_engine = [&c](u32 m, std::initializer_list<u32> a,
                                SubmissionMode mode = SubmissionMode::Increasing) {
      c.push_back(Header(m, u32(a.size()), mode, 1));
      for (u32 v : a)
        c.push_back(CommandHeader{v});
    };
    // Two lines of 0x30 bytes, 0x100 apart
    c.clear();
    on_engine(0x60, {0x30, 2, 0x1, 0x3000, 0x100});
    on_engine(0x6C, {1});
    on_engine(0x6D, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                     22, 23, 24},
              SubmissionMode::NonIncreasing);
    e.ProcessCommands(c);
    Check(e.posted.size() == 2 && is(e.posted[0], Kind::Nothing) &&
              is(e.posted[1], Kind::Range, 0x100003000, 0x130),
          "a linear inline upload declared the wrong range");
    c.clear();
    e.posted.clear();
    on_engine(0x6C, {0});
    on_engine(0x6D, {1}, SubmissionMode::NonIncreasing);
    e.ProcessCommands(c);
    Check(e.posted.size() == 2 && is(e.posted[1], Kind::Anywhere),
          "a block linear inline upload stopped waiting");
    if (type == Engines::EngineTypes::KeplerCompute) {
      c.clear();
      e.posted.clear();
      on_engine(0xAF, {1});
      e.ProcessCommands(c);
      Check(e.posted.size() == 1 && is(e.posted[0], Kind::Nothing) && e.queued_dispatch,
            "a compute launch waited, or a SYNC_WAIT entry would not wait for it");
      // After a dispatch wrote commands read before it ran, or with the setting, every
      // launch is waited for
      for (int strict = 0; strict < 2; ++strict) {
        c.clear();
        e.posted.clear();
        e.queued_dispatch = false;
        e.strict_dispatch_wait = strict == 0;
        e.posted_dispatch = 0;
        Settings::strict_compute_ordering = strict == 1;
        on_engine(0xAF, {1});
        e.ProcessCommands(c);
        Check(e.posted.size() == 1 && is(e.posted[0], Kind::Anywhere) && !e.queued_dispatch &&
                  e.posted_dispatch == 0,
              "a strict compute launch did not wait");
      }
      e.strict_dispatch_wait = false;
      Settings::strict_compute_ordering = false;
    }
  }

  // MaxwellDMA declares its destination and its semaphore
  {
    using DmaRegs = Engines::MaxwellDMA::Regs;
    constexpr u32 launch = offsetof(DmaRegs, launch_dma) / 4;
    constexpr u32 offset_out = offsetof(DmaRegs, offset_out) / 4;
    constexpr u32 semaphore = offsetof(DmaRegs, semaphore) / 4;
    constexpr u32 dst_params = offsetof(DmaRegs, dst_params) / 4;
    constexpr u32 PitchDst = 1U << 8, MultiLine = 1U << 9, OneWordSemaphore = 1U << 3;
    UploadEngine engine;
    engine.execution_mask[launch] = true;
    Dma e;
    e.threaded = true;
    e.subchannels[2] = &engine;
    e.subchannel_type[2] = Engines::EngineTypes::MaxwellDMA;
    const auto on_engine = [&c](u32 m, std::initializer_list<u32> a) {
      c.push_back(Header(m, u32(a.size()), SubmissionMode::Increasing, 2));
      for (u32 v : a)
        c.push_back(CommandHeader{v});
    };
    const auto copy = [&](std::initializer_list<u32> registers, u32 flags) {
      c.clear();
      e.posted.clear();
      // offset_out, pitch_in, pitch_out, line_length_in, line_count
      on_engine(offset_out, registers);
      on_engine(launch, {flags});
      e.ProcessCommands(c);
      Check(e.posted.size() == 1, "MaxwellDMA registers made decode wait");
      return e.posted.back();
    };
    // One line ignores line_count and stays in its GOBs
    Check(is(copy({0x1, 0x3010, 0, 0, 0x30, 0}, PitchDst), Kind::Range, 0x100003000, 0x200),
          "a one line copy declared the wrong range");
    Check(is(copy({0x1, 0x3010, 0, 0x100, 0x40, 4}, PitchDst | MultiLine), Kind::Range,
              0x100003010, 0x400),
          "a pitch copy declared the wrong range");
    Check(is(copy({0x1, 0x3010, 0, 0xFFFFFF00, 0x40, 4}, PitchDst | MultiLine), Kind::Anywhere),
          "a negative pitch copy stopped waiting");
    c.clear();
    on_engine(dst_params, {0x10, 0x100, 16, 1, 0, 0});
    on_engine(semaphore, {0x1, 0x5000});
    e.ProcessCommands(c);
    const Dma::BackWrites block = copy({0x1, 0x8000, 0, 0, 0x100, 16}, MultiLine | OneWordSemaphore);
    Check(is(block, Kind::Range, 0x100008000, 0x1000) && block.extra_address == 0x100005000 &&
              block.extra_size == 16,
          "a block linear copy with a semaphore declared the wrong ranges");
    Check(is(copy({0x1, 0x8000, 0, 0, 0, 16}, MultiLine | OneWordSemaphore), Kind::Range,
              0x100005000, 16),
          "an empty copy did not declare its semaphore");
    c.clear();
    on_engine(dst_params + 4, {1});
    e.ProcessCommands(c);
    Check(is(copy({0x1, 0x8000, 0, 0, 0x100, 16}, MultiLine), Kind::Anywhere),
          "a layered block linear copy stopped waiting");
  }

  // Fermi2D declares its destination surface
  {
    using BlitRegs = Engines::Fermi2D::Regs;
    constexpr u32 dst = offsetof(BlitRegs, dst) / 4;
    constexpr u32 rectangle = offsetof(BlitRegs, pixels_from_memory.dst_x0) / 4;
    constexpr u32 trigger = offsetof(BlitRegs, pixels_from_memory.src_y0) / 4 + 1;
    UploadEngine engine;
    engine.execution_mask[trigger] = true;
    Dma e;
    e.threaded = true;
    e.subchannels[3] = &engine;
    e.subchannel_type[3] = Engines::EngineTypes::Fermi2D;
    const auto on_engine = [&c](u32 m, std::initializer_list<u32> a) {
      c.push_back(Header(m, u32(a.size()), SubmissionMode::Increasing, 3));
      for (u32 v : a)
        c.push_back(CommandHeader{v});
    };
    const auto blit = [&](std::initializer_list<u32> surface, std::initializer_list<u32> rect) {
      c.clear();
      e.posted.clear();
      // format, linear, block, depth, layer, pitch, width, height, address
      on_engine(dst, surface);
      on_engine(rectangle, rect);
      on_engine(trigger, {0});
      e.ProcessCommands(c);
      Check(e.posted.size() == 1, "Fermi2D registers made decode wait");
      return e.posted.back();
    };
    Check(is(blit({0, 1, 0, 1, 0, 0x400, 0x100, 8, 0x1, 0x6000}, {0, 0, 0x100, 8}), Kind::Range,
              0x100006000, 0x3000),
          "a pitch blit declared the wrong range");
    // 64 pixels of up to 16 bytes, 16 lines of blocks two GOBs high
    Check(is(blit({0, 0, 0x10, 1, 0, 0, 64, 10, 0x1, 0x6000}, {0, 0, 64, 10}), Kind::Range,
              0x100006000, 0x4000),
          "a block linear blit declared the wrong range");
    Check(is(blit({0, 0, 0x10, 1, 0, 0, 64, 10, 0x1, 0x6000}, {0, 0, 64, 20}), Kind::Range,
              0x100006000, 0x8000),
          "a blit out of its surface declared the wrong range");
    Check(is(blit({0, 0, 0, 1, 1, 0, 64, 16, 0x1, 0x6000}, {0, 0, 64, 16}), Kind::Anywhere),
          "a layered blit stopped waiting");
  }
}

// Maxwell3D's unpublished write ranges, checked against a GP entry's device ranges
struct WriteRanges {
  struct Memory {
    // GPU 0x10000-0x20000 maps to device 0x900000; GPU 0x30000-0x31000 aliases its first page
    std::vector<std::pair<DAddr, std::size_t>> GetSubmappedDeviceRange(GPUVAddr a,
                                                                       std::size_t n) const {
      if (a >= 0x10000 && a + n <= 0x20000)
        return {{0x900000 + (a - 0x10000), n}};
      if (a >= 0x30000 && a + n <= 0x31000)
        return {{0x900000 + (a - 0x30000), n}};
      return {};
    }
  } memory_manager;
  bool stream_writes_memory{}, stream_writes_anywhere{};
  std::vector<std::pair<GPUVAddr, GPUVAddr>> stream_write_ranges;
  std::vector<std::pair<DAddr, DAddr>> device_write_ranges;
  size_t converted_write_ranges{};
  DAddr device_writes_begin{~DAddr{}};
  DAddr device_writes_end{};
  bool UnpublishedMayWrite(std::span<const std::pair<DAddr, std::size_t>> ranges);
  void ConvertWriteRanges();
  void ClearWriteRanges();
  void FrontMemoryWrite(GPUVAddr address, u64 size);
};
#include "writes.inc"

// Two subchannels bound to one MaxwellDMA engine keep their own register writes until each
// launches: the declared destination is the one the queued launch uses when it runs
struct CopyEngine : Engines::EngineInterface {
  std::array<u32, 0x800> regs{};
  u32 launch{};
  std::vector<GPUVAddr> destinations;
  void CallMethod(u32 method, u32 argument, bool) override {
    regs[method] = argument;
    if (method == launch) {
      using Regs = Engines::MaxwellDMA::Regs;
      constexpr u32 offset_out = offsetof(Regs, offset_out) / 4;
      destinations.push_back((GPUVAddr{regs[offset_out]} << 32) | regs[offset_out + 1]);
    }
  }
  void CallMultiMethod(u32 method, const u32 *base, u32 amount, u32) override {
    for (u32 i = 0; i < amount; ++i)
      CallMethod(method, base[i], true);
  }
};

// The decode side check of a fetched entry against the dispatches still queued
struct Probe {
  static constexpr unsigned FetchedAddressBits = 34;
  static constexpr unsigned FetchedPageBits = 12;
  mutable std::vector<u64> fetched_pages =
      std::vector<u64>((u64{1} << (FetchedAddressBits - FetchedPageBits)) / 64);
  struct KnownPage {
    u64 page{~u64{0}};
    u64 mapping_generation{};
    u64 compute_write_epoch{};
  };
  static constexpr size_t KnownPageCount = 1024;
  mutable std::vector<KnownPage> known_pages = std::vector<KnownPage>(KnownPageCount);
  mutable bool queued_dispatch{};
  mutable u64 launched_dispatches{};
  mutable std::atomic<u64> completed_dispatches{};
  struct UnwaitedEntry {
    std::atomic<u64> address{}, size{}, first{}, last{};
  };
  static constexpr size_t UnwaitedEntryCount = 2048;
  mutable std::array<UnwaitedEntry, UnwaitedEntryCount> unwaited_entries{};
  mutable std::atomic<u64> unwaited_count{};
  struct Memory {
    u64 generation{};
    u64 MappingGeneration() const { return generation; }
    // GPU 0x10000 is device 0x200000
    std::vector<std::pair<DAddr, std::size_t>> GetSubmappedDeviceRange(GPUVAddr a, size_t s) const {
      return {{a + 0x1F0000, s}};
    }
  } memory_manager;
  struct Raster {
    std::vector<std::pair<DAddr, DAddr>> written;
    u64 epoch{};
    bool MayHaveComputeWrites(DAddr a, u64 s) {
      return std::ranges::any_of(written, [a, s](auto w) { return a < w.second && a + s > w.first; });
    }
    u64 ComputeWriteEpoch() { return epoch; }
  } raster;
  Raster *rasterizer = &raster;
  bool FetchesNewPages(DAddr, u64) const;
  bool IsKnownPushbuffer(GPUVAddr, size_t) const;
  bool DispatchMayWrite(GPUVAddr, size_t, std::span<const std::pair<DAddr, std::size_t>>) const;
  void RecordUnwaitedEntry(GPUVAddr, size_t) const;
  bool UnwaitedEntryWritten(u64) const;
};
#include "probe.inc"

void QueuedDispatchTests() {
  using Ranges = std::vector<std::pair<DAddr, std::size_t>>;
  Probe p;
  // A queued dispatch, an entry at GPU 0x10000 backed by device 0x200000
  const Ranges entry{{0x200000, 0x100}};
  p.queued_dispatch = true;
  // Fetched for the first time it may be the dispatch output
  Check(!p.IsKnownPushbuffer(0x10000, 0x100) && p.DispatchMayWrite(0x10000, 0x100, entry),
        "a first fetch did not wait for a queued dispatch");
  // Fetched before and never written by a dispatch, it is known
  Check(p.IsKnownPushbuffer(0x10000, 0x100) && !p.DispatchMayWrite(0x10000, 0x100, entry),
        "a pushbuffer no dispatch wrote waited");
  // A dispatch writes its page: the epoch changes and the history has it
  p.raster.written.push_back({0x200000, 0x201000});
  ++p.raster.epoch;
  Check(!p.IsKnownPushbuffer(0x10000, 0x100) && p.DispatchMayWrite(0x10000, 0x100, entry),
        "an entry a dispatch wrote before did not wait");
  Check(!p.IsKnownPushbuffer(0x10000, 0x100), "an entry a dispatch wrote was kept as known");
  // Without a queued dispatch nothing waits, but the pages are recorded
  const Ranges other{{0x400000, 0x80}};
  p.queued_dispatch = false;
  Check(!p.DispatchMayWrite(0x30000, 0x80, other), "an entry waited without a queued dispatch");
  p.queued_dispatch = true;
  Check(p.IsKnownPushbuffer(0x30000, 0x80) && !p.DispatchMayWrite(0x30000, 0x80, other),
        "an entry fetched without a queued dispatch was not recorded");
  // A remap forgets the known pages
  ++p.memory_manager.generation;
  Check(!p.IsKnownPushbuffer(0x30000, 0x80), "a remapped page was kept as known");
  Check(!p.DispatchMayWrite(0x30000, 0x80, other) && p.IsKnownPushbuffer(0x30000, 0x80),
        "a remapped clean page was not known again");
  // An entry over two pages is known only when both are
  const Ranges two{{0x400F80, 0x80}, {0x401000, 0x80}};
  Check(!p.IsKnownPushbuffer(0x30F80, 0x100) && p.DispatchMayWrite(0x30F80, 0x100, two),
        "an entry reaching a page fetched for the first time did not wait");
  Check(p.IsKnownPushbuffer(0x30F80, 0x100), "an entry over two known pages was not known");

  // Entry E at GPU 0x10000 (device 0x200000) is read while dispatch 1 is queued
  {
    // Dispatch 1 writes E: E was read too early
    Probe u;
    u.launched_dispatches = 1;
    u.RecordUnwaitedEntry(0x10000, 0x100);
    u.RecordUnwaitedEntry(0x50000, 0x100);
    Check(!u.UnwaitedEntryWritten(1), "entries no dispatch wrote were detected");
    u.raster.written.push_back({0x200000, 0x201000});
    Check(u.UnwaitedEntryWritten(1), "an earlier dispatch writing an entry read before it ran was missed");
  }
  {
    // E launches dispatch 2, which writes E's page for later: E was read in order
    Probe u;
    u.launched_dispatches = 1;
    u.RecordUnwaitedEntry(0x10000, 0x100);
    u.launched_dispatches = 2;
    Check(!u.UnwaitedEntryWritten(1), "a dispatch that wrote nothing was blamed");
    u.completed_dispatches = 1;
    u.raster.written.push_back({0x200000, 0x201000});
    Check(!u.UnwaitedEntryWritten(2), "a dispatch launched by the entry it writes was blamed");
  }
  {
    // Safe DMA: the queue ran before the read, nothing is recorded
    Probe u;
    u.launched_dispatches = 1;
    u.completed_dispatches = 1;
    u.RecordUnwaitedEntry(0x10000, 0x100);
    u.raster.written.push_back({0x200000, 0x201000});
    Check(u.unwaited_count == 0 && !u.UnwaitedEntryWritten(1),
          "an entry read after the queued dispatches ran was recorded");
  }
  {
    // Old entries leave the ring
    Probe w;
    w.launched_dispatches = 1;
    w.RecordUnwaitedEntry(0x10000, 0x100);
    for (int i = 0; i < 2100; ++i) {
      w.RecordUnwaitedEntry(0x60000 + i * 0x100, 0x100);
    }
    w.raster.written.push_back({0x200000, 0x201000});
    Check(!w.UnwaitedEntryWritten(1), "an entry overwritten in the ring was still checked");
  }

  // The buffer and image caches mark what a dispatch writes
  VideoCommon::ComputeWriteHistory history;
  history.Mark(0x200000, 0x2000);
  Check(history.Epoch() == 1 && history.MayHaveWrites(0x201FFF, 1) &&
            !history.MayHaveWrites(0x202000, 0x1000),
        "a dispatch write was not marked by page");
  history.Mark(0x200800, 0x100);
  history.Mark(0x200000, 0x2000);
  Check(history.Epoch() == 1, "marking written pages again changed the epoch");
  history.Mark(0x201000, 0x2000);
  Check(history.Epoch() == 2 && history.MayHaveWrites(0x202000, 0x1000),
        "a write reaching a new page did not change the epoch");
}

void SharedEngineTests() {
  using Kind = Dma::BackWrites::Kind;
  using Regs = Engines::MaxwellDMA::Regs;
  constexpr u32 launch = offsetof(Regs, launch_dma) / 4;
  constexpr u32 offset_out = offsetof(Regs, offset_out) / 4;
  constexpr u32 line_length_in = offsetof(Regs, line_length_in) / 4;
  constexpr u32 PitchDst = 1U << 8;
  CopyEngine engine;
  engine.launch = launch;
  engine.execution_mask[launch] = true;
  Dma d;
  d.threaded = true;
  d.defer = true;
  for (const u32 channel : {2U, 4U}) {
    d.subchannels[channel] = &engine;
    d.subchannel_type[channel] = Engines::EngineTypes::MaxwellDMA;
  }
  std::vector<CommandHeader> c;
  const auto on = [&c](u32 channel, u32 m, std::initializer_list<u32> a) {
    c.push_back(Header(m, u32(a.size()), SubmissionMode::Increasing, channel));
    for (u32 v : a)
      c.push_back(CommandHeader{v});
  };
  // A sets X, B sets Y, A launches, then B launches
  on(2, offset_out, {0x1, 0x3000});
  on(2, line_length_in, {0x40});
  on(4, offset_out, {0x1, 0x7000});
  on(4, line_length_in, {0x40});
  on(2, launch, {PitchDst});
  on(4, launch, {PitchDst});
  d.ProcessCommands(c);
  Check(d.posted.size() == 2 && d.posted[0].kind == Kind::Range && d.posted[1].kind == Kind::Range,
        "shared engine copies did not declare their ranges");
  d.RunQueued();
  Check(engine.destinations == std::vector<GPUVAddr>{0x100003000, 0x100007000},
        "the queued launches used unexpected registers");
  for (size_t i = 0; i < 2; ++i) {
    const GPUVAddr x = engine.destinations[i];
    Check(d.posted[i].address <= x && x < d.posted[i].address + d.posted[i].size,
          "a copy declared another subchannel's destination");
  }
}

void UnpublishedWritesTests() {
  WriteRanges w;
  using Ranges = std::vector<std::pair<DAddr, std::size_t>>;
  const auto may = [&w](DAddr a, std::size_t n) { return w.UnpublishedMayWrite(Ranges{{a, n}}); };
  w.FrontMemoryWrite(0x10100, 0x10);
  Check(!may(0x900100, 4), "a stream without memory writes made decode publish");
  w.stream_writes_memory = true;
  Check(may(0x900108, 4), "an unpublished write to the entry was missed");
  Check(!may(0x900110, 0x10), "a write next to the entry made decode publish");
  Check(!may(0x800000, 0x1000), "a write elsewhere made decode publish");
  // Contiguous with a converted range: kept apart so it is converted too
  w.FrontMemoryWrite(0x10110, 0x10);
  Check(may(0x900118, 4), "a write after a converted one was missed");
  Check(w.device_write_ranges.size() == 2, "a converted range was extended");
  // Written through another GPU mapping of the same device memory
  w.FrontMemoryWrite(0x30400, 8);
  Check(may(0x900400, 8), "a write through an aliased mapping was missed");
  Check(may(0x900000, 0x1000) && !may(0x901000, 0x1000), "bounds of the writes are wrong");
  w.stream_writes_anywhere = true;
  Check(may(0x800000, 4), "a stream that may write anywhere did not make decode publish");
  w.stream_writes_anywhere = false;
  w.ClearWriteRanges();
  Check(!may(0x900108, 4) && w.converted_write_ranges == 0, "cleared writes still made decode publish");
  w.FrontMemoryWrite(0x10200, 4);
  Check(may(0x900200, 4), "a write after a clear was missed");
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
      Check(!a.observed.empty(), "queued executable methods did not run");
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
    f.DrainStream();
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
void QueueTests();
int main(int argc, char **) {
  Tests();
  MacroStreamTests();
  UniformPrefetchStopTests();
  QueuedWritesTests();
  QueuedDispatchTests();
  SharedEngineTests();
  UnpublishedWritesTests();
  QueueTests();
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
