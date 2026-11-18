// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_common/common_types.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
unsigned checks{};
void Check(bool ok, const char *text) {
  ++checks;
  if (!ok)
    throw std::runtime_error(text);
}
enum class CacheType { TextureCache = 1 };
struct TICEntry {
  u8 raw[32];
};
struct TSCEntry {
  u8 raw[32];
};
struct DeviceMemory {
  std::map<DAddr, int> cached;
  std::vector<std::pair<DAddr, u64>> flush_regions;
  void UpdatePagesCachedCount(DAddr addr, size_t size, int delta) {
    for (DAddr page = addr & ~DAddr{0xFFF}; page < addr + size; page += 0x1000)
      cached[page] += delta;
  }
  void RegisterFlushCacheRegion(DAddr addr, u64 size, CacheType) {
    flush_regions.emplace_back(addr, size);
  }
  bool Registered(DAddr addr) const {
    return std::ranges::any_of(flush_regions, [&](const std::pair<DAddr, u64> &r) {
      return r.first <= addr && addr < r.first + r.second;
    });
  }
};
struct GpuMemory {
  std::map<GPUVAddr, DAddr> pages;
  std::optional<DAddr> GpuToCpuAddress(GPUVAddr addr) const {
    const auto it = pages.find(addr & ~GPUVAddr{0xFFF});
    if (it == pages.end())
      return std::nullopt;
    return it->second + (addr & 0xFFF);
  }
};
// Fixture for the descriptor table tracking of TextureCache; only memory is replaced
struct Tables {
  DeviceMemory device_memory;
  GpuMemory gpu;
  GpuMemory *gpu_memory = &gpu;
  int channel{};
  const void *channel_state = &channel;
  u64 descriptor_epoch = 1;
  const void *tracked_channel{};
  GPUVAddr tracked_tic_addr{};
  GPUVAddr tracked_tsc_addr{};
  u32 tracked_tic_limit{};
  u32 tracked_tsc_limit{};
  bool tables_tracked{};
  bool tables_checked{};
  std::vector<std::pair<DAddr, size_t>> table_ranges;
#include "descriptor_members.inc"
  void TrackGraphicsDescriptorTables(GPUVAddr, u32, GPUVAddr, u32);
  void UntrackGraphicsDescriptorTables();
  bool OverlapsDescriptorTables(DAddr, size_t) const noexcept;
  bool OverlapsDescriptorTablesGPU(GPUVAddr, size_t) const noexcept;
  void NotifyUnsafeWrite(GPUVAddr, size_t);
};
#include "descriptors.inc"

constexpr GPUVAddr TIC = 0x10000, TSC = 0x20000;
constexpr DAddr TIC_DEVICE = 0x500000, TSC_DEVICE = 0x700000;
Tables Mapped() {
  Tables t;
  t.gpu.pages = {{TIC, TIC_DEVICE},
                 {TSC, TSC_DEVICE},
                 {0x30000, 0x900000},
                 {0x40000, TIC_DEVICE}};
  return t;
}
// Each draw synchronizes the tables first, then compares the epoch with its previous draw
u64 Draw(Tables &t) {
  t.TrackGraphicsDescriptorTables(TIC, 127, TSC, 15);
  return t.GraphicsDescriptorEpoch();
}
void Tests() {
  {
    Tables t = Mapped();
    const u64 first = Draw(t);
    Check(first != 0, "tables were not tracked");
    Check(t.device_memory.cached[TIC_DEVICE] == 1 && t.device_memory.cached[TSC_DEVICE] == 1,
          "table pages were not counted as cached");
    Check(t.device_memory.Registered(TIC_DEVICE) && t.device_memory.Registered(TSC_DEVICE + 0x1FF),
          "table pages were not registered in the flush cache filter");
    Check(Draw(t) == first, "an unchanged draw did not reuse");

    t.NotifyUnsafeWrite(0x30000, 0x1000);
    Check(Draw(t) == first, "a DMA write elsewhere stopped reuse");
    t.NotifyUnsafeWrite(TSC + 0x10, 16);
    const u64 after_tsc = Draw(t);
    Check(after_tsc != first, "a DMA write to the sampler table was reused");
    t.NotifyUnsafeWrite(0x40100, 4);
    const u64 after_alias = Draw(t);
    Check(after_alias != after_tsc, "a DMA write through an alias of the texture table was reused");
    t.NotifyUnsafeWrite(TSC - 0x800, 0x1000);
    Check(Draw(t) != after_alias, "a DMA write from an unmapped page into a table was reused");

    Check(!t.OverlapsDescriptorTablesGPU(0x30000, 0x10000), "an unrelated mapping change moved the tables");
    Check(t.OverlapsDescriptorTablesGPU(TIC + 0xFF0, 0x20), "a mapping change of the texture table was missed");
    Check(t.OverlapsDescriptorTablesGPU(TSC - 0x10, 0x11), "a mapping change of the sampler table was missed");
    Check(!t.OverlapsDescriptorTablesGPU(TSC + 0x200, 0x1000), "the sampler table limit was ignored");

    t.UntrackGraphicsDescriptorTables();
    Check(t.GraphicsDescriptorEpoch() == 0, "untracked tables kept reuse on");
    Check(t.device_memory.cached[TIC_DEVICE] == 0 && t.device_memory.cached[TSC_DEVICE] == 0,
          "untracking left the table pages cached");
    t.NotifyUnsafeWrite(TSC, 4);
    Check(Draw(t) != 0, "tables were not tracked again");
  }
  {
    Tables t = Mapped();
    t.gpu.pages.erase(TSC);
    Check(Draw(t) == 0, "a table with an unmapped page was tracked");
    Check(t.device_memory.cached[TIC_DEVICE] == 0, "an untrackable table left pages cached");
    Check(t.table_ranges.empty(), "an untrackable table kept its ranges");
  }
  {
    Tables t = Mapped();
    t.TrackGraphicsDescriptorTables(TIC, (4u << 20) / 32, TSC, 15);
    Check(t.GraphicsDescriptorEpoch() == 0, "a table over 4 MiB was tracked");
  }
  {
    Tables t = Mapped();
    const u64 first = Draw(t);
    int other_channel{};
    t.channel_state = &other_channel;
    Check(Draw(t) != first, "a channel change kept reuse");
  }
  std::printf("descriptor tables: %u checks passed\n", checks);
}
int main() {
  try {
    Tests();
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
