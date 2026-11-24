// SPDX-License-Identifier: GPL-2.0-or-later
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/guest_memory.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

VideoSettings videoSettings{};

namespace {

constexpr u64 Page = Core::DEVICE_PAGESIZE;
constexpr DAddr Base = 0x100000;
constexpr DAddr AliasBase = 0x200000;
constexpr VAddr CpuBase = 0x10000;
constexpr VAddr OtherCpuBase = 0x40000;

unsigned checks{};

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(what);
    }
}

struct RecordingMemory : TestMemory {
    std::map<u64, bool> cached;
    std::map<u64, s32> write_protected;
    void RasterizerMarkRegionCached(uint64_t vaddr, uint64_t size, bool cached_) override {
        for (u64 page = vaddr / Page; page < (vaddr + size) / Page; ++page) {
            cached[page] = cached_;
        }
    }
    void RasterizerMarkRegionWriteProtected(uint64_t vaddr, uint64_t size, int32_t delta) override {
        for (u64 page = vaddr / Page; page < (vaddr + size) / Page; ++page) {
            write_protected[page] += delta;
        }
    }
};

struct InPlaceRasterizer : TestRasterizer {
    Tegra::MaxwellDeviceMemoryManager* device{};
    std::vector<u64> waits;
    void WaitInPlaceReads(DAddr addr, u64 size) override {
        device->ReleaseInPlaceReads(addr, size, [this](u64 tick) { waits.push_back(tick); });
    }
};

struct Fixture {
    RecordingMemory memory;
    InPlaceRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{memory};
    Core::Asid asid;

    explicit Fixture(bool enable = true) : asid(device.RegisterProcess(&memory)) {
        rasterizer.device = &device;
        device.BindInterface(&rasterizer);
        if (enable) {
            device.EnableInPlaceReads();
        }
        device.Map(Base, CpuBase, 2 * Page, asid, true);
    }

    bool Cached(u64 page = 0) {
        return memory.cached[CpuBase / Page + page];
    }
    s32 WriteProtected(u64 page = 0, VAddr cpu_base = CpuBase) {
        return memory.write_protected[cpu_base / Page + page];
    }
};

void TestHoldAndRelease() {
    Fixture f;
    f.device.HoldInPlaceRead(Base + 0x100, 0x200, 7);
    f.device.HoldInPlaceRead(Base + 0x400, 0x200, 8);
    Check(f.Cached() && f.WriteProtected() == 1, "a page read in place is cached and protected");

    std::vector<u64> waits;
    f.device.ReleaseInPlaceReads(Base + 0x800, 4, [&](u64 tick) { waits.push_back(tick); });
    Check(waits == std::vector<u64>{8}, "a write waits for the last tick that read the page");
    Check(!f.Cached() && f.WriteProtected() == 0, "the released page is no longer protected");
    Check(f.device.IsExcludedFromInPlace(Base, 4), "a rewritten page takes the copy path from then on");

    waits.clear();
    f.device.ReleaseInPlaceReads(Base, Page, [&](u64 tick) { waits.push_back(tick); });
    Check(waits.empty(), "a released page doesn't wait again");
}

void TestBoundAgainWhileWaiting() {
    Fixture f;
    f.device.HoldInPlaceRead(Base, 0x100, 3);
    std::vector<u64> waits;
    f.device.ReleaseInPlaceReads(Base, 4, [&](u64 tick) {
        waits.push_back(tick);
        if (tick == 3) {
            f.device.HoldInPlaceRead(Base, 0x100, 5);
        }
    });
    Check(waits == std::vector<u64>{3, 5}, "a read in place during the wait is waited for");
    Check(!f.Cached() && f.WriteProtected() == 0, "the page is released after both reads");
}

void TestBoundAgainWhileReleasing() {
    constexpr u32 Rounds = 200000;
    Fixture f;
    std::atomic<u32> round{};
    std::atomic<u32> released{};
    u64 waited{};
    std::jthread writer([&] {
        for (u32 r = 1; r <= Rounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
            }
            f.device.ReleaseInPlaceReads(Base, 4, [&](u64 tick) { waited = std::max(waited, tick); });
            released.store(r, std::memory_order_release);
        }
    });
    u32 lost{};
    for (u32 r = 1; r <= Rounds; ++r) {
        waited = 0;
        f.device.HoldInPlaceRead(Base, 0x100, 2 * r);
        round.store(r, std::memory_order_release);
        // Sweeps the moment of the new read across the write
        for (volatile u32 delay = r % 256; delay != 0; delay = delay - 1) {
        }
        const u64 tick = 2 * r + 1;
        f.device.HoldInPlaceRead(Base, 0x100, tick);
        while (released.load(std::memory_order_acquire) != r) {
        }
        if (f.WriteProtected() == 0 && waited < tick) {
            ++lost;
        }
        f.device.ReleaseInPlaceReads(Base, 4, [](u64) {});
    }
    Check(lost == 0, "a read in place racing a write keeps the page protected");
}

void TestSecondWriteWhileReleasing() {
    constexpr u32 Rounds = 100000;
    Fixture f;
    std::atomic<u32> round{};
    std::atomic<u32> bound{};
    std::atomic<u32> first_done{};
    std::atomic<u32> second_done{};
    std::atomic<u64> waited{};
    std::atomic<u32> lost{};
    const auto record = [&](u64 tick) {
        u64 seen = waited.load();
        while (seen < tick && !waited.compare_exchange_weak(seen, tick)) {
        }
    };
    std::jthread first([&] {
        for (u32 r = 1; r <= Rounds; ++r) {
            while (round.load(std::memory_order_acquire) != r) {
            }
            f.device.ReleaseInPlaceReads(Base, 4, record);
            first_done.store(r, std::memory_order_release);
        }
    });
    std::jthread second([&] {
        for (u32 r = 1; r <= Rounds; ++r) {
            while (bound.load(std::memory_order_acquire) != r) {
            }
            f.device.ReleaseInPlaceReads(Base, 4, record);
            if (waited.load() < 2 * r + 1) {
                ++lost;
            }
            second_done.store(r, std::memory_order_release);
        }
    });
    for (u32 r = 1; r <= Rounds; ++r) {
        waited = 0;
        f.device.HoldInPlaceRead(Base, 0x100, 2 * r);
        round.store(r, std::memory_order_release);
        for (volatile u32 delay = r % 256; delay != 0; delay = delay - 1) {
        }
        f.device.HoldInPlaceRead(Base, 0x100, 2 * r + 1);
        bound.store(r, std::memory_order_release);
        while (first_done.load(std::memory_order_acquire) != r ||
               second_done.load(std::memory_order_acquire) != r) {
        }
        f.device.ReleaseInPlaceReads(Base, 4, [](u64) {});
    }
    Check(lost == 0, "a write after a new read in place waits for it");
}

void TestPartialRelease() {
    Fixture f;
    f.device.HoldInPlaceRead(Base, 2 * Page, 4);
    f.device.ReleaseInPlaceReads(Base + Page, 4, [](u64) {});
    Check(f.WriteProtected(0) == 1 && f.WriteProtected(1) == 0, "the other page stays held");
    Check(!f.device.IsExcludedFromInPlace(Base, Page), "the other page can still be read in place");
}

void TestEngineWriteWaits() {
    Fixture f;
    f.device.HoldInPlaceRead(Base, 0x100, 6);
    f.device.PrepareEngineWrite(Base + 0x80, 4);
    Check(f.rasterizer.waits == std::vector<u64>{6}, "an engine write waits for the read in place");
    Check(f.WriteProtected() == 0 && f.device.IsExcludedFromInPlace(Base, 4),
          "an engine write releases the page and marks it");
}

void TestEngineWriteToExcludedPageHeldAgain() {
    Fixture f;
    f.device.PrepareEngineWrite(Base, 4);
    f.device.HoldInPlaceRead(Base, 0x100, 8);
    f.device.PrepareEngineWrite(Base + 0x80, 4);
    Check(f.rasterizer.waits == std::vector<u64>{8} && f.WriteProtected() == 0,
          "an engine write to an excluded page waits for a read in place held since");
}

void TestAliasedEngineWrite() {
    Fixture f;
    f.device.Map(AliasBase, CpuBase, Page, f.asid, true);
    f.device.HoldInPlaceRead(AliasBase, 0x100, 12);
    f.device.PrepareEngineWrite(Base + 0x10, 4);
    Check(f.rasterizer.waits == std::vector<u64>{12} && f.WriteProtected() == 0,
          "an engine write through an alias waits for the read in place");
    Check(f.device.IsExcludedFromInPlace(AliasBase, 4), "the alias takes the copy path too");
}

void TestWritableSpan() {
    Fixture f;
    f.device.HoldInPlaceRead(Base, 0x100, 14);
    {
        Tegra::Memory::DeviceGuestMemory<u8, GuestMemoryFlags::UnsafeRead> read(f.device, Base, 4);
    }
    Check(f.rasterizer.waits.empty(), "reading guest memory doesn't wait");
    Tegra::Memory::DeviceGuestMemoryScoped<u8, GuestMemoryFlags::UnsafeReadWrite> span(f.device,
                                                                                      Base, 4);
    Check(f.rasterizer.waits == std::vector<u64>{14} && f.WriteProtected() == 0,
          "a writable span waits for the read in place");
}

void TestGpuWritableSpan() {
    Fixture f;
    Tegra::MemoryManager gpu{f.device, 32, 1ULL << 32};
    gpu.BindRasterizer(&f.rasterizer);
    constexpr GPUVAddr GpuBase = 0x10000000;
    gpu.Map(GpuBase, Base, Page, Tegra::PTEKind::INVALID, false);
    f.device.HoldInPlaceRead(Base, 0x100, 15);
    Tegra::Memory::GpuGuestMemoryScoped<u8, GuestMemoryFlags::UnsafeReadWrite> span(gpu, GpuBase,
                                                                                   4);
    Check(f.rasterizer.waits == std::vector<u64>{15} && f.WriteProtected() == 0,
          "a writable span at a GPU address waits for the read in place");
    Check(f.device.IsEngineWritten(Base, 4), "a writable span at a GPU address marks its page");
}

void TestRemap() {
    Fixture f;
    f.device.HoldInPlaceRead(Base, 0x100, 9);
    f.device.Unmap(Base, 2 * Page);
    Check(f.rasterizer.waits == std::vector<u64>{9} && f.WriteProtected() == 0,
          "an unmap waits for the read in place and lifts its protection");

    f.device.Map(Base, OtherCpuBase, 2 * Page, f.asid, true);
    Check(!f.device.IsExcludedFromInPlace(Base, 2 * Page), "a new mapping can be read in place");
    f.device.HoldInPlaceRead(Base, 0x100, 10);
    Check(f.WriteProtected(0, OtherCpuBase) == 1, "a read in place protects the new backing");

    f.device.Map(Base, CpuBase, 2 * Page, f.asid, true);
    Check(f.WriteProtected(0, OtherCpuBase) == 0 && f.WriteProtected() == 0,
          "mapping over a held page releases the old backing");
}

void TestDisabled() {
    Fixture f{false};
    f.device.PrepareEngineWrite(Base, 4);
    f.device.ReleaseInPlaceReads(Base, 4, [](u64) { throw std::runtime_error("waited"); });
    Check(!f.device.InPlaceReadsEnabled() && f.rasterizer.waits.empty(), "writes skip the tracking");
}

void TestEngineWrittenPages() {
    Fixture f{false};
    Check(!f.device.IsEngineWritten(Base, 2 * Page), "pages start unwritten by engines");
    f.device.PrepareEngineWrite(Base + 8, 4);
    Check(f.device.IsEngineWritten(Base, 4) && !f.device.IsEngineWritten(Base + Page, 4),
          "an engine write marks its pages without a guest RAM import");
    const u32 value = 1;
    f.device.WriteBlockUnsafe(Base + Page, &value, sizeof(value));
    Check(f.device.IsEngineWritten(Base + Page, 4), "a device memory write marks its pages");

    Fixture g{false};
    g.device.Map(AliasBase, CpuBase, Page, g.asid, true);
    g.device.PrepareEngineWrite(Base, 4);
    Check(g.device.IsEngineWritten(AliasBase, 4), "an engine write marks the aliases too");

    Fixture h{false};
    h.device.Map(AliasBase, CpuBase, Page, h.asid, true);
    h.device.WriteBlockUnsafe(AliasBase + 0x100, &value, sizeof(value));
    Check(h.device.IsEngineWritten(Base, 4), "a device memory write marks the aliases too");
}

void TestExclusion() {
    Fixture f;
    Check(!f.device.IsExcludedFromInPlace(Base, 2 * Page), "pages start unwritten");
    f.device.ExcludeFromInPlace(Base + Page + 8, 4);
    Check(f.device.IsExcludedFromInPlace(Base, 2 * Page), "a range holding a written page is written");
    Check(!f.device.IsExcludedFromInPlace(Base, Page), "the page before is not");
    Check(f.device.IsExcludedFromInPlace(~DAddr{0} - 4, 4), "an address outside the device space can't be read in place");
}

} // namespace

int main() {
    try {
        TestHoldAndRelease();
        TestBoundAgainWhileWaiting();
        TestBoundAgainWhileReleasing();
        TestSecondWriteWhileReleasing();
        TestPartialRelease();
        TestEngineWriteWaits();
        TestEngineWriteToExcludedPageHeldAgain();
        TestAliasedEngineWrite();
        TestWritableSpan();
        TestGpuWritableSpan();
        TestRemap();
        TestExclusion();
        TestDisabled();
        TestEngineWrittenPages();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("in_place_reads: %u checks passed\n", checks);
    return 0;
}
