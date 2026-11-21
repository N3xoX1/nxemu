// SPDX-License-Identifier: GPL-2.0-or-later
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/range_sets.inc"
#include "yuzu_video_core/buffer_cache/gpu_modified_ranges.h"
#include <cstdio>
#include <map>
#include <stdexcept>

VideoSettings videoSettings{};

namespace {

constexpr u64 Page = Core::DEVICE_PAGESIZE;
constexpr DAddr Base = 0x100000;
constexpr VAddr CpuBase = 0x10000;

unsigned checks{};

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(what);
    }
}

// The page state the OS side applies: cached pages report CPU writes, write protected
// pages report them exactly
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

struct Fixture {
    RecordingMemory memory;
    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{memory};
    Core::Asid asid;
    VideoCommon::GpuModifiedRanges<DAddr, Page> ranges;

    Fixture() : asid(device.RegisterProcess(&memory)) {
        device.BindInterface(&rasterizer);
        device.Map(Base, CpuBase, Page, asid, true);
        ranges.SetOnPages([this](DAddr addr, u64 size, s32 delta) {
            device.UpdatePagesGpuModifiedCount(addr, size, delta);
        });
    }

    bool Cached() {
        return memory.cached[CpuBase / Page];
    }
    s32 WriteProtected() {
        return memory.write_protected[CpuBase / Page];
    }
};

// The GPU owns bytes at the end of a page, and the CPU writes its start: the buffer tracker
// untracks the page. A second CPU write, into the GPU-owned bytes, must still be reported.
void TestCpuWriteNextToGpuBytes() {
    Fixture f;
    f.device.UpdatePagesCachedCount(Base, Page, 1);
    f.ranges.Add(Base + 0xf00, 0x100);
    Check(f.Cached() && f.WriteProtected() == 1, "a page with GPU-written bytes is write protected");

    f.device.UpdatePagesCachedCount(Base, Page, -1);
    Check(f.Cached(), "the page stays cached after the first CPU write untracks it");
    Check(f.WriteProtected() == 1, "the page stays write protected after the first CPU write");

    f.ranges.Subtract(Base + 0xf00, 0x100);
    Check(!f.Cached() && f.WriteProtected() == 0, "the last GPU-written range releases the page");
}

// A GPU write to a page the buffer tracker doesn't hold caches it until the range is released
void TestGpuWriteOnUntrackedPage() {
    Fixture f;
    f.ranges.Add(Base + 0x10, 0x20);
    Check(f.Cached() && f.WriteProtected() == 1, "GPU-written bytes cache an untracked page");
    f.device.UpdatePagesCachedCount(Base, Page, 1);
    f.ranges.Subtract(Base, Page);
    Check(f.Cached() && f.WriteProtected() == 0, "the buffer tracker keeps the page cached");
}

} // namespace

int main() {
    try {
        TestCpuWriteNextToGpuBytes();
        TestGpuWriteOnUntrackedPage();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("gpu_modified_protection: %u checks passed\n", checks);
    return 0;
}
