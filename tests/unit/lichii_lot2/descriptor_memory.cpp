// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <stdexcept>
#include "../fast_copy/fixture.h"
#include "yuzu_video_core/texture_cache/descriptor_table.h"

struct Descriptor {
    std::array<u64, 4> words{};
    bool operator==(const Descriptor&) const = default;
};
static_assert(sizeof(Descriptor) == 32);
void Check(bool ok) {
    if (!ok) throw std::runtime_error("descriptor memory regression");
}
void Compare(CopyFixture& fixture, GPUVAddr address) {
    VideoCommon::DescriptorTable<Descriptor> table{fixture.gpu};
    Check(table.Synchronize(address, 0));
    Descriptor reference;
    fixture.gpu.ReadBlockUnsafe(address, &reference, sizeof(reference));
    const auto [actual, changed] = table.Read(0);
    Check(actual == reference && changed);
    Check(!table.Read(0).second);
    table.Invalidate();
    Check(table.Read(0).second && table.Read(0).first == reference);
}
int main() {
    CopyFixture fixture;
    for (size_t i = 0; i < fixture.physical.bytes.size(); ++i)
        fixture.physical.bytes[i] = static_cast<u8>((i * 17 + i / 4096) & 255U);
    const auto source = CopyFixture::Source;
    for (u64 offset : {0ULL, 1ULL, 4064ULL, 4080ULL, 4095ULL, 65520ULL})
        Compare(fixture, source + offset);
    // Adjacent GPU pages may map to entirely separate physical pages.
    constexpr GPUVAddr fragmented = 0x6000000;
    fixture.gpu.Map(fragmented, source, 4096, Tegra::PTEKind::INVALID, false);
    fixture.gpu.Map(fragmented + 4096, source + 32768, 4096, Tegra::PTEKind::INVALID, false);
    for (u64 offset : {0ULL, 4064ULL, 4080ULL, 4095ULL, 4096ULL})
        Compare(fixture, fragmented + offset);
    fixture.gpu.MapSparse(0x8000000, 65536);
    Compare(fixture, 0x8000000); // Reserved sparse mapping: descriptors read as zero.
    Compare(fixture, 0x9000000); // Entirely unmapped GPU address.
    Compare(fixture, fragmented + 8192 - 16); // Mapped first half, unmapped second half.
    // A big GPU mapping can cover a fragmented device-memory mapping.
    constexpr DAddr scattered_device = 0x3000000;
    const auto asid = fixture.device.RegisterProcess(&fixture.physical);
    for (u64 page = 0; page < 16; ++page)
        fixture.device.Map(scattered_device + page * 4096, source + page * 8192, 4096, asid, false);
    fixture.gpu.Map(0xa000000, scattered_device, 65536);
    for (u64 offset : {0ULL, 4064ULL, 4080ULL, 4096ULL, 8192ULL})
        Compare(fixture, 0xa000000 + offset);
    VideoCommon::DescriptorTable<Descriptor> table{fixture.gpu};
    Check(table.Synchronize(source, 0));
    Check(table.Read(0).second && !table.Read(0).second);
    fixture.physical.bytes[source] ^= 1;
    Check(table.Read(0).second && !table.Read(0).second);
    Check(fixture.rasterizer.flushes == 0); // Preserve the unsafe reader's no-flush contract.
}
