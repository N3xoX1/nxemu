// SPDX-FileCopyrightText: 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Compile the real SlotVector and DescriptorTable headers with a tiny GPU-memory stub.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "yuzu_common/slot_vector.h"
#include "yuzu_video_core/texture_cache/descriptor_table.h"

struct TestValue {
    int value{};
    explicit TestValue(int value_) : value{value_} {}
    TestValue(TestValue&&) noexcept = default;
    TestValue& operator=(TestValue&&) noexcept = default;
};

void TestSlotVector() {
    Common::SlotVector<TestValue> slots;
    assert(slots.begin() == slots.end());

    std::vector<Common::SlotId> ids;
    std::map<u32, int> expected;
    for (int i = 0; i < 1536; ++i) {
        const auto id = slots.insert(i);
        ids.push_back(id);
        expected[id.index] = i;
    }

    auto check = [&] {
        auto it = slots.begin();
        for (const auto& [id, value] : expected) {
            assert(it != slots.end());
            const auto before = it++;
            const auto [actual_id, object] = *before;
            assert(actual_id.index == id);
            assert(object->value == value);
        }
        assert(it == slots.end());
        ++it;
        assert(it == slots.end());

        auto expected_it = expected.begin();
        slots.ForEachOccupied([&](TestValue& object) {
            assert(expected_it != expected.end());
            assert(object.value == expected_it->second);
            ++expected_it;
        });
        assert(expected_it == expected.end());
    };

    check();
    std::mt19937 random{0x12345678};
    std::shuffle(ids.begin(), ids.end(), random);
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i % 31 != 0) {
            slots.erase(ids[i]);
            expected.erase(ids[i].index);
        }
    }
    check();

    // Exercise empty words, sparse first and last slots, and reuse after deletion.
    for (int i = 0; i < 300; ++i) {
        const auto id = slots.insert(2000 + i);
        expected[id.index] = 2000 + i;
    }
    check();
    for (const auto& [id, value] : expected) {
        (void)value;
        slots.erase(Common::SlotId{id});
    }
    expected.clear();
    check();
    const auto id = slots.insert(42);
    expected[id.index] = 42;
    check();
}

struct TestDescriptor {
    u64 data{};
    bool operator==(const TestDescriptor&) const = default;
};

void TestDescriptorTable() {
    Tegra::MemoryManager memory{4096};
    VideoCommon::DescriptorTable<TestDescriptor> table{memory};
    assert(table.Synchronize(0, 128));

    for (u32 index : {0U, 63U, 64U, 127U, 128U}) {
        memory.Write(index * sizeof(TestDescriptor), TestDescriptor{index + 10});
        const auto [first, changed] = table.Read(index);
        assert(changed && first.data == index + 10);
        assert(!table.Read(index).second);
    }
    assert(memory.FallbackReads() == 0);
    // Both repeated invalidations and the first subsequent read must be correct.
    table.Invalidate();
    table.Invalidate();
    assert(table.Read(63).second);
    assert(!table.Read(63).second);
    assert(table.Read(64).second);
    assert(!table.Read(64).second);
    // A changed descriptor is still discovered without invalidation.
    memory.Write(64 * sizeof(TestDescriptor), TestDescriptor{1000});
    assert(table.Read(64).second);
    assert(!table.Read(64).second);

    // Synchronizing an unchanged table must preserve the invalidated bitmap.
    table.Invalidate();
    assert(!table.Synchronize(0, 128));
    assert(table.Read(64).second);

    // Refresh installs a fresh bitmap, even when a previous table was invalidated.
    table.Invalidate();
    assert(table.Synchronize(2048, 31));
    memory.Write(2048, TestDescriptor{73});
    assert(table.Read(0).second);
    assert(!table.Read(0).second);
    assert(table.Synchronize(0, 128));
    assert(table.Read(128).second);

    // Unmapped and non-backed entries still take the original safe fallback.
    memory.SetDirectAccess(false);
    memory.Write(64 * sizeof(TestDescriptor), TestDescriptor{1001});
    const auto fallback_before = memory.FallbackReads();
    const auto [fallback, changed] = table.Read(64);
    assert(fallback.data == 1001 && changed);
    assert(memory.FallbackReads() == fallback_before + 1);
    memory.SetDirectAccess(true);

    // A descriptor crossing a 4 KiB mapping boundary must not use GetPointer.
    Tegra::MemoryManager boundary_memory{8192};
    VideoCommon::DescriptorTable<TestDescriptor> boundary_table{boundary_memory};
    assert(boundary_table.Synchronize(4092, 0));
    boundary_memory.Write(4092, TestDescriptor{0x123456789ABCDEF0ULL});
    assert(boundary_table.Read(0).first.data == 0x123456789ABCDEF0ULL);
    assert(boundary_memory.FallbackReads() == 1);
}

int main() {
    TestSlotVector();
    TestDescriptorTable();
}
