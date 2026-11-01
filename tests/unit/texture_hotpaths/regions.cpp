// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_common/hash.h"
#include "yuzu_common/slot_vector.h"
#include "yuzu_video_core/texture_cache/image_base.h"
#include <algorithm>
#include <array>
#include <boost/container/small_vector.hpp>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using VideoCommon::ImageFlagBits;
using VideoCommon::ImageId;
using VideoCommon::ImageMapId;
unsigned checks{};
void Check(bool ok, const char *text) {
  ++checks;
  if (!ok)
    throw std::runtime_error(text);
}
constexpr u64 YUZU_PAGEBITS = 20, PAGE = u64{1} << YUZU_PAGEBITS;
struct Image {
  GPUVAddr gpu_addr{};
  size_t guest_size_bytes{};
  ImageFlagBits flags{};
  bool OverlapsGPU(GPUVAddr a, size_t n) const {
    return gpu_addr < a + n && a < gpu_addr + guest_size_bytes;
  }
};
struct ImageMapView {
  GPUVAddr gpu_addr{};
  DAddr cpu_addr{};
  size_t size{};
  ImageId image_id{};
  bool picked{};
  bool Overlaps(DAddr a, size_t n) const {
    return cpu_addr < a + n && a < cpu_addr + size;
  }
};
struct Region {
  Common::SlotVector<Image> slot_images;
  Common::SlotVector<ImageMapView> slot_map_views;
  std::unordered_map<u64, std::vector<ImageMapId>, Common::IdentityHash<u64>>
      page_table;
  using GpuTable =
      std::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>;
  std::array<GpuTable, 2> gpu_page_table_storage;
  std::vector<ImageId> images;
  std::vector<ImageMapId> maps;
  std::optional<size_t> getStorageID(size_t as_id) {
    return as_id == 0 ? std::optional<size_t>{0} : std::nullopt;
  }
#include "pages.inc"
  template <class Func> void ForEachImageInRegion(DAddr, size_t, Func &&);
  template <class Func>
  void ForEachImageInRegionGPU(size_t, GPUVAddr, size_t, Func &&);
  ImageId Add(u64 cpu, u64 gpu, size_t n, ImageId alias = {}) {
    ImageId image = alias;
    if (!image) {
      image = slot_images.insert();
      slot_images[image] = {gpu, n, {}};
      images.push_back(image);
      ForEachGPUPage(gpu, n, [&](u64 page) {
        gpu_page_table_storage[0][page].push_back(image);
      });
    }
    auto map = slot_map_views.insert();
    slot_map_views[map] = {gpu, cpu, n, image, false};
    maps.push_back(map);
    ForEachCPUPage(cpu, n, [&](u64 page) { page_table[page].push_back(map); });
    return image;
  }
  void Clean() {
    for (auto id : images)
      Check(False(slot_images[id].flags & ImageFlagBits::Picked),
            "picked image leaked");
    for (auto id : maps)
      Check(!slot_map_views[id].picked, "picked map leaked");
  }
};
struct Reference : Region {
  template <class Func> void ForEachImageInRegion(DAddr, size_t, Func &&);
  template <class Func>
  void ForEachImageInRegionGPU(size_t, GPUVAddr, size_t, Func &&);
};
struct Eden : Region {
  template <class Func> void ForEachImageInRegion(DAddr, size_t, Func &&);
  template <class Func>
  void ForEachImageInRegionGPU(size_t, GPUVAddr, size_t, Func &&);
};
#include "eden-regions.inc"
#include "reference-regions.inc"
#include "regions.inc"
template <class Cache>
std::vector<u32> Query(Cache &c, bool gpu, u64 address, u64 size,
                       int stop = -1) {
  std::vector<u32> result;
  auto callback = [&](ImageId id, Image &) {
    result.push_back(id.index);
    return stop >= 0 && int(result.size()) >= stop;
  };
  if (gpu)
    c.ForEachImageInRegionGPU(0, address, size, callback);
  else
    c.ForEachImageInRegion(address, size, callback);
  return result;
}
void Tests() {
  Region current;
  Reference before;
  Eden eden;
  auto a = current.Add(100, 100, 1000), b = before.Add(100, 100, 1000),
       e = eden.Add(100, 100, 1000);
  current.Add(200, 100, 200, a);
  before.Add(200, 100, 200, b);
  eden.Add(200, 100, 200, e);
  Check(Query(current, false, 250, 1) == Query(before, false, 250, 1),
        "CPU aliases differed");
  Check(Query(eden, false, 250, 1).size() == 2 &&
            Query(before, false, 250, 1).size() == 1,
        "fork duplicate alias counterexample changed");
  current.slot_images[a].flags |= ImageFlagBits::Picked;
  Check(Query(current, true, 250, 1).empty(), "GPU outer picked flag ignored");
  Check(Query(current, false, 250, 1).empty(), "CPU outer picked flag ignored");
  current.slot_images[a].flags &= ~ImageFlagBits::Picked;
  current.Clean();
  Check(Query(before, false, 250, 0).size() == 1 &&
            Query(current, false, 250, 0).empty(),
        "zero-size CPU false callback counterexample changed");
  Check(Query(before, true, 250, 0).size() == 1 &&
            Query(current, true, 250, 0).empty(),
        "zero-size GPU false callback counterexample changed");
  Region single;
  single.Add(10, 10, 200);
  unsigned calls{};
  single.ForEachImageInRegion(20, 1, [&](ImageId, Image &) {
    ++calls;
    Check(Query(single, false, 20, 1).empty(),
          "nested CPU query reentered active image");
    Check(Query(single, true, 20, 1).empty(),
          "nested GPU query reentered active image");
  });
  Check(calls == 1, "single page callback count");
  single.Clean();
  Check(Query(single, false, 0, 0).empty() && Query(single, true, 0, 0).empty(),
        "empty region must not enumerate");
  Check(Query(single, false, PAGE + 1, 1).empty() &&
            Query(single, true, PAGE + 1, 1).empty(),
        "missing page callback");
  single.slot_map_views[single.maps[0]].picked = true;
  Check(Query(single, false, 20, 1).empty(), "prepicked map ignored");
  single.slot_map_views[single.maps[0]].picked = false;
  // ID reuse and replacement are observed on the next query, with no persistent
  // fast-path cache.
  auto image = single.images[0];
  auto map = single.maps[0];
  single.slot_images.erase(image);
  single.slot_map_views.erase(map);
  single.images.clear();
  single.maps.clear();
  single.page_table.clear();
  single.gpu_page_table_storage[0].clear();
  auto replacement = single.Add(PAGE + 10, PAGE + 10, 50);
  Check(Query(single, false, 20, 1).empty() &&
            Query(single, false, PAGE + 20, 1) ==
                std::vector<u32>{replacement.index},
        "recreated resource used stale page/ID");
  std::mt19937 rng(0x4c4f5434);
  for (unsigned i = 0; i < 96; ++i) {
    u64 address = rng() % (8 * PAGE);
    u64 length = 1 + rng() % (2 * PAGE);
    auto x = current.Add(address, address, length);
    auto y = before.Add(address, address, length);
    if (i % 3 == 0) {
      current.Add(address + 1, address, 1, x);
      before.Add(address + 1, address, 1, y);
    }
  }
  for (unsigned i = 0; i < 12000; ++i) {
    u64 address = rng() % (10 * PAGE), size = 1 + rng() % (2 * PAGE);
    int stop = i % 3 == 0 ? 1 : -1;
    for (bool gpu : {false, true})
      Check(Query(current, gpu, address, size, stop) ==
                Query(before, gpu, address, size, stop),
            "random query/order/early break differs");
    current.Clean();
    before.Clean();
  }
  Region boundary;
  Reference old;
  boundary.Add(PAGE - 10, PAGE - 10, 20);
  old.Add(PAGE - 10, PAGE - 10, 20);
  for (u64 n : {1ULL, 10ULL, 11ULL, 20ULL})
    Check(Query(boundary, false, PAGE - 10, n) ==
              Query(old, false, PAGE - 10, n),
          "page boundary duplication");
  for (size_t as : {0u, 1u}) {
    unsigned count{};
    boundary.ForEachImageInRegionGPU(as, PAGE, 1,
                                     [&](ImageId, Image &) { ++count; });
    Check(count == (as == 0 ? 1 : 0), "address space selection");
  }
  std::fprintf(stderr,
               "regions: %u checks passed, 24000 differential queries\n",
               checks);
}
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
template <class Cache>
NOINLINE u64 Scan(Cache &c, bool gpu, u64 address, u64 size) {
  u64 count{};
  auto callback = [&](ImageId id, Image &) { count += id.index + 1; };
  if (gpu)
    c.ForEachImageInRegionGPU(0, address, size, callback);
  else
    c.ForEachImageInRegion(address, size, callback);
  return count;
}
volatile u64 sink{};
template <class Cache> double Bench(bool gpu, int scenario) {
  Cache c;
  if (scenario == 1)
    c.Add(10, 10, 64);
  if (scenario == 2)
    for (unsigned i = 0; i < 8; ++i)
      c.Add(10 + i * 100, 10 + i * 100, 64);
  if (scenario == 3)
    c.Add(PAGE - 10, PAGE - 10, 2 * PAGE);
  const u64 address = scenario == 3 ? PAGE - 1 : 10,
            size = scenario == 3 ? PAGE + 2 : 1024;
  constexpr unsigned iterations = 200000;
  u64 total{};
  auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < iterations; ++i)
    total += Scan(c, gpu, address, size);
  auto end = std::chrono::steady_clock::now();
  sink = total;
  Check(total == u64(iterations) * (scenario == 0   ? 0
                                    : scenario == 2 ? 36
                                                    : 1),
        "benchmark callback checksum");
  return std::chrono::duration<double, std::nano>(end - start).count() /
         iterations;
}
int main(int argc, char **) {
  try {
    Tests();
    if (argc > 1) {
      std::puts("gpu,scenario,sample,baseline_ns,candidate_ns");
      for (bool gpu : {false, true})
        for (int scenario = 0; scenario < 4; ++scenario)
          for (unsigned sample = 0; sample < 9; ++sample) {
            double a, b;
            if (sample % 2) {
              a = Bench<Region>(gpu, scenario);
              b = Bench<Reference>(gpu, scenario);
            } else {
              b = Bench<Reference>(gpu, scenario);
              a = Bench<Region>(gpu, scenario);
            }
            std::printf("%u,%d,%u,%.3f,%.3f\n", unsigned(gpu), scenario, sample,
                        b, a);
          }
    }
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
