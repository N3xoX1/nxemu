// SPDX-License-Identifier: GPL-2.0-or-later
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>

VideoSettings videoSettings{};

namespace {

constexpr u64 BigPage = 0x10000;
constexpr u64 DevicePage = 0x1000;
constexpr DAddr ContiguousDevice = 0x100000;
constexpr DAddr OtherDevice = 0x400000;
constexpr VAddr ContiguousCpu = 0x100000;
constexpr VAddr OtherCpu = 0x800000;
constexpr GPUVAddr BigPages = 0x10000000;
constexpr GPUVAddr OtherBigPage = BigPages + 2 * BigPage;
constexpr GPUVAddr SmallPage = 0x20000000;
constexpr DAddr SmallPageDevice = ContiguousDevice + 4 * BigPage;
constexpr GPUVAddr Unmapped = 0x30000000;

unsigned checks{};

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        throw std::runtime_error(what);
    }
}

struct Fixture {
    TestMemory physical;
    TestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::MemoryManager gpu{device, 32, 1ULL << 32};

    Fixture() {
        device.BindInterface(&rasterizer);
        const auto asid = device.RegisterProcess(&physical);
        device.Map(ContiguousDevice, ContiguousCpu, 8 * BigPage, asid, true);
        device.Map(OtherDevice, OtherCpu, BigPage, asid, true);
        gpu.BindRasterizer(&rasterizer);
        gpu.Map(BigPages, ContiguousDevice, 2 * BigPage);
        gpu.Map(OtherBigPage, OtherDevice, BigPage);
        gpu.Map(SmallPage, SmallPageDevice, DevicePage, Tegra::PTEKind::INVALID, false);
    }

    bool Holds(VAddr cpu_addr, const void* data, size_t size) {
        return std::memcmp(physical.bytes.data() + cpu_addr, data, size) == 0;
    }
};

constexpr std::array<u32, 4> Data{0x11111111, 0x22222222, 0x33333333, 0x44444444};
constexpr size_t DataSize = sizeof(Data);

void TestWithinOneDevicePage() {
    Fixture f;
    const auto device_addr = f.gpu.WriteConstBufferCached(BigPages + 0x100, Data.data(), DataSize);
    Check(device_addr == ContiguousDevice + 0x100, "an upload in one device page returns its address");
    Check(f.Holds(ContiguousCpu + 0x100, Data.data(), DataSize), "an upload in one device page is written");
    Check(f.device.IsEngineWritten(ContiguousDevice + 0x100, DataSize), "an upload marks its page");
}

void TestAcrossDevicePages() {
    Fixture f;
    const GPUVAddr address = BigPages + DevicePage - 8;
    const auto device_addr = f.gpu.WriteConstBufferCached(address, Data.data(), DataSize);
    Check(device_addr == ContiguousDevice + DevicePage - 8,
          "an upload across contiguous device pages returns its address");
    Check(f.Holds(ContiguousCpu + DevicePage - 8, Data.data(), DataSize),
          "an upload across device pages is written");
    Check(f.device.IsEngineWritten(ContiguousDevice + DevicePage, 4),
          "an upload across device pages marks both");
}

void TestAcrossDiscontinuity() {
    Fixture f;
    const GPUVAddr address = OtherBigPage - 8;
    const auto device_addr = f.gpu.WriteConstBufferCached(address, Data.data(), DataSize);
    Check(!device_addr, "an upload across a discontinuity returns no address");
    Check(f.Holds(ContiguousCpu + 2 * BigPage - 8, Data.data(), 8) &&
              f.Holds(OtherCpu, Data.data() + 2, 8),
          "an upload across a discontinuity is written to both ranges");
}

void TestSmallPage() {
    Fixture f;
    const auto device_addr = f.gpu.WriteConstBufferCached(SmallPage + 0x20, Data.data(), DataSize);
    Check(device_addr == SmallPageDevice + 0x20, "an upload to a small page returns its address");
    Check(f.Holds(ContiguousCpu + 4 * BigPage + 0x20, Data.data(), DataSize),
          "an upload to a small page is written");
}

void TestUnmapped() {
    Fixture f;
    Check(!f.gpu.WriteConstBufferCached(Unmapped, Data.data(), DataSize),
          "an upload to unmapped memory returns no address");
}

} // namespace

int main() {
    try {
        TestWithinOneDevicePage();
        TestAcrossDevicePages();
        TestAcrossDiscontinuity();
        TestSmallPage();
        TestUnmapped();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("constant_uploads: %u checks passed\n", checks);
    return 0;
}
