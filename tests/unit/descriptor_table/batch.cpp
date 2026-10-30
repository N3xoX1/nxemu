#include <array>
#include <cassert>
#include "yuzu_video_core/texture_cache/descriptor_table.h"

int main() {
    Tegra::MemoryManager memory;
    using Descriptor = std::array<u32, 8>;
    VideoCommon::DescriptorTable<Descriptor> table{memory};
    assert(table.Synchronize(256, 7));
    Descriptor first{};
    first[0] = 123;
    std::memcpy(memory.bytes.data() + 256, &first, sizeof(first));
    table.BeginReadBatch();
    assert(!table.BatchEntryUnchanged(0));
    assert(!table.BatchEntryUnchanged(1)); // Unread entries cannot reuse a cached null view.
    assert(memory.reads == 1);
    assert(table.Read(0).first == first);
    assert(!table.Read(0).second);
    assert(memory.reads == 1);
    Descriptor replacement{};
    replacement[0] = 456;
    std::memcpy(memory.bytes.data() + 256, &replacement, sizeof(replacement));
    // The snapshot belongs only to this pass, including repeated lookups.
    assert(table.Read(0).first == first);
    table.EndReadBatch();
    assert(!table.BatchedDescriptor(0));
    const auto changed = table.Read(0);
    assert(changed.first == replacement && changed.second);
    table.BeginReadBatch();
    assert(table.BatchEntryUnchanged(0));
    assert(!table.BatchEntryUnchanged(1));
    assert(table.Read(0).first == replacement);
    assert(!table.Read(0).second);
    table.EndReadBatch();
    table.Invalidate();
    assert(table.Read(0).second);
    table.BeginReadBatch();
    assert(table.Synchronize(512, 3));
    assert(!table.BatchedDescriptor(0));
    assert(table.Read(0).first == Descriptor{});
    assert(table.BeginReadBatch());
    for (u32 i = 0; i < 4; ++i) (void)table.Read(i);
    table.EndReadBatch();
    assert(!table.BeginReadBatch()); // Dense lookup switches back to scalar reads.
    assert(table.Read(0).first == Descriptor{});
    assert(table.Synchronize(768, 3));
    assert(table.BeginReadBatch()); // A new table does not inherit the cooldown.
    table.EndReadBatch();
    // Differential workload: mutate entries across block boundaries and compare
    // every returned descriptor with guest memory through dense fallback/retries.
    for (unsigned mode : {0U, 1U, 2U}) {
    Tegra::MemoryManager changing;
    changing.contiguous = mode == 1;
    changing.fragmented = mode == 2;
    if (changing.fragmented) changing.blocked_span_page = 1;
    changing.bytes.resize(256 + 512 * sizeof(Descriptor));
    VideoCommon::DescriptorTable<Descriptor> changing_table{changing};
    assert(changing_table.Synchronize(256, 511));
    u32 random = 12345;
    for (u32 pass = 0; pass < 100; ++pass) {
        random = random * 1664525U + 1013904223U;
        const u32 index = random % 512;
        Descriptor value{};
        value[0] = pass; value[7] = random;
        std::memcpy(changing.bytes.data() + 256 + index * sizeof(Descriptor), &value, sizeof(value));
        if (pass % 13 == 0) changing_table.Invalidate();
        const unsigned reads_before = changing.reads;
        const bool active = changing_table.BeginReadBatch();
        if (active && changing.contiguous) assert(changing.reads == reads_before);
        if (active) {
            const std::array<Descriptor, 128> zeros{};
            for (u32 block = 0; block < 512; block += 128) {
                const bool empty = std::memcmp(changing.bytes.data() + 256 + block * sizeof(Descriptor),
                                               zeros.data(), sizeof(zeros)) == 0;
                assert(changing_table.BatchBlockEmpty(block) == empty);
                if (pass % 13 == 0) assert(!changing_table.BatchBlockUnchanged(block));
            }
            assert(!changing_table.BatchBlockEmpty(1));
            assert(!changing_table.BatchBlockEmpty(512));
        }
        for (u32 i = 0; i < 512; ++i) {
            Descriptor expected;
            std::memcpy(&expected, changing.bytes.data() + 256 + i * sizeof(Descriptor), sizeof(expected));
            assert(changing_table.Read(i).first == expected);
        }
        if (active) changing_table.EndReadBatch();
    }
    }
    Tegra::MemoryManager live;
    live.contiguous = true;
    VideoCommon::DescriptorTable<Descriptor> live_table{live};
    assert(live_table.Synchronize(256, 7));
    std::memcpy(live.bytes.data() + 256, &first, sizeof(first));
    assert(live_table.Read(0).first == first);
    assert(live_table.BeginReadBatch());
    assert(live_table.BatchEntryUnchanged(0));
    std::memcpy(live.bytes.data() + 256, &replacement, sizeof(replacement));
    assert(live_table.Read(0).first == first); // Never retains the guest pointer.
    live_table.EndReadBatch();

    // Invalid image headers can keep nonzero auxiliary words with a null address.
    // Resolving them without a view lookup must still warm the canonical bytes.
    Tegra::MemoryManager headers;
    headers.contiguous = true;
    headers.bytes.resize(256 + 256 * sizeof(Descriptor));
    VideoCommon::DescriptorTable<Descriptor> header_table{headers};
    assert(header_table.Synchronize(256, 255));
    for (u32 index = 0; index < 256; ++index) {
        Descriptor value{}; value[7] = index + 1;
        std::memcpy(headers.bytes.data() + 256 + index * sizeof(Descriptor), &value, sizeof(value));
    }
    assert(header_table.BeginReadBatch());
    assert(!header_table.BatchBlockEmpty(0));
    for (u32 index = 0; index < 256; ++index) header_table.CommitBatchEntry(index);
    header_table.EndReadBatch();
    assert(header_table.BeginReadBatch());
    assert(header_table.BatchBlockUnchanged(0));
    assert(header_table.BatchBlockUnchanged(128));
    assert(!header_table.BatchBlockEmpty(0));
    header_table.EndReadBatch();
    std::memset(headers.bytes.data() + 256, 0, 128 * sizeof(Descriptor));
    assert(header_table.BeginReadBatch());
    assert(header_table.BatchBlockEmpty(0));
    assert(!header_table.BatchBlockUnchanged(0));
    header_table.CommitBatchBlock(0);
    assert(header_table.BatchBlockUnchanged(0));
    assert(header_table.Read(0).first == Descriptor{});
    assert(!header_table.Read(0).second);
    header_table.EndReadBatch();
    // A scalar lookup outside a batch must invalidate cached empty-block knowledge.
    std::memcpy(headers.bytes.data() + 256, &first, sizeof(first));
    assert(header_table.Read(0).first == first);
    assert(header_table.BeginReadBatch());
    assert(!header_table.BatchBlockEmpty(0));
    header_table.Invalidate();
    assert(!header_table.BatchBlockUnchanged(0));
    header_table.EndReadBatch();

    for (bool blocked_tail : {false, true}) {
        Tegra::MemoryManager segmented;
        segmented.fragmented = true;
        segmented.bytes.resize(8192);
        if (blocked_tail) segmented.blocked_span_page = 1;
        VideoCommon::DescriptorTable<Descriptor> segments{segmented};
        assert(segments.Synchronize(256, 127));
        std::array<Descriptor, 128> expected{};
        for (size_t index = 0; index < expected.size(); ++index) {
            expected[index][0] = static_cast<u32>(index + 1);
            std::memcpy(segmented.bytes.data() + 256 + index * sizeof(Descriptor),
                        &expected[index], sizeof(Descriptor));
            assert(segments.Read(static_cast<u32>(index)).first == expected[index]);
        }
        const auto begin = [&] {
            for (unsigned retry = 0; retry < 65; ++retry) {
                if (segments.BeginReadBatch()) return;
            }
            assert(false);
        };
        for (unsigned pass = 0; pass < 4; ++pass) {
            const size_t changed = pass % 2 ? 0 : 127;
            expected[changed][7] = pass + 123;
            std::memcpy(segmented.bytes.data() + 256 + changed * sizeof(Descriptor),
                        &expected[changed], sizeof(Descriptor));
            begin();
            for (size_t index = 0; index < expected.size(); ++index) {
                assert(segments.Read(static_cast<u32>(index)).first == expected[index]);
            }
            segments.EndReadBatch();
        }
    }
    assert(live_table.BeginReadBatch());
    assert(live_table.Read(0).first == replacement);
    live_table.EndReadBatch();
}
