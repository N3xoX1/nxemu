#include <algorithm>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include "yuzu_video_core/renderer_vulkan/descriptor_table_writer.h"

template <class T> T Handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
bool Equal(const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
    return a.imageView == b.imageView && a.sampler == b.sampler && a.imageLayout == b.imageLayout;
}
int main() {
    std::vector<VkDescriptorImageInfo> entries;
    Vulkan::DescriptorTableWriterState metadata;
    const VkDescriptorImageInfo null{VK_NULL_HANDLE, Handle<VkImageView>(1), VK_IMAGE_LAYOUT_GENERAL};
    for (size_t pass = 0; pass < 100; ++pass) {
        const size_t count = pass < 40 ? 512 : pass < 60 ? 128 : 1024;
        std::vector<VkDescriptorImageInfo> expected(count, null);
        for (size_t i = 0; i < count; i += 37) {
            expected[i].imageView = Handle<VkImageView>(i + 100 + pass / 5);
        }
        // Exercise sampler-only and layout-only changes as well as image handles.
        expected[3].sampler = Handle<VkSampler>(pass / 7 + 1);
        expected[7].imageLayout = pass % 10 < 5 ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        const bool changed = !std::ranges::equal(entries, expected, Equal);
        const auto previous = entries;
        Vulkan::DescriptorTableWriter writer{entries, &metadata};
        const auto base = writer.Reserve(count - 1, null);
        for (size_t i = 0; i + 1 < count; ++i) writer.Write(base + i, expected[i]);
        writer.Append(expected.back());
        writer.Finish();
        assert(writer.Changed() == changed);
        assert(std::ranges::equal(entries, expected, Equal));
        if (!changed) assert(std::ranges::equal(entries, previous, Equal));
    }
    Vulkan::DescriptorTableWriter empty{entries};
    empty.Finish();
    assert(empty.Changed() && entries.empty());

    entries.assign(256, null);
    metadata = {};
    const auto run = [&](size_t count, size_t start, size_t length,
                         const VkDescriptorImageInfo& value, bool expect_changed) {
        Vulkan::DescriptorTableWriter writer{entries, &metadata};
        writer.Reserve(count, null);
        writer.WriteRun(start, length, value);
        writer.Finish();
        assert(writer.Changed() == expect_changed);
        for (size_t i = start; i < start + length; ++i) assert(Equal(entries[i], value));
    };
    run(256, 0, 128, null, false);
    run(256, 128, 128, null, false);
    auto different = null;
    different.imageView = Handle<VkImageView>(234);
    {
        Vulkan::DescriptorTableWriter writer{entries, &metadata};
        writer.Reserve(256, null);
        writer.Write(17, different); // Invalidates the previously constant block.
        writer.Finish();
        assert(writer.Changed());
    }
    run(256, 0, 128, null, true);
    run(256, 0, 128, null, false);
    run(256, 0, 64, different, true); // Partial writes cannot mark a whole block constant.
    run(256, 0, 128, null, true);
    run(64, 0, 64, null, true); // Shrink through a partial block.
    run(256, 0, 128, null, true); // Regrowth must not reuse a partial-block claim.
    run(256, 0, 128, different, true);
    // Capture immutable recipes while the worker lags behind several draws.
    // Every array has its own default; sampler-only tails and null restoration
    // must survive expansion without consulting the producer's mutable scratch.
    Vulkan::DescriptorTablePlan plan;
    std::vector<std::shared_ptr<const Vulkan::DescriptorTablePlan>> queued;
    std::vector<std::vector<VkDescriptorImageInfo>> references;
    const auto legacy_before = entries;
    for (size_t pass = 0; pass < 100; ++pass) {
        Vulkan::DescriptorTableWriter writer{entries, &metadata, &plan};
        const size_t count = pass % 3 ? 4096 : 127;
        const auto first = writer.Reserve(count, null);
        std::vector<VkDescriptorImageInfo> expected(count, null);
        for (size_t i = 0; i < count; i += pass % 5 ? 97 : 1) {
            auto value = different;
            value.imageView = Handle<VkImageView>(1000 + i + pass);
            writer.Write(first + i, value);
            expected[i] = value;
        }
        writer.WriteRun(first, count, null); // Restore previously overwritten entries.
        std::fill(expected.begin(), expected.end(), null);
        auto live = different;
        live.imageView = Handle<VkImageView>(50000 + pass);
        writer.Write(first + 3, live);
        expected[3] = live;
        const size_t second = writer.Reserve(129, different);
        expected.resize(count + 129, different);
        writer.Write(second + 128, null);
        expected.back() = null;
        auto sampler = null;
        sampler.sampler = Handle<VkSampler>(pass + 1);
        sampler.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        writer.Append(sampler);
        expected.push_back(sampler);
        writer.Finish();
        assert(std::ranges::equal(entries, legacy_before, Equal));
        queued.push_back(std::make_shared<const Vulkan::DescriptorTablePlan>(plan));
        references.push_back(std::move(expected));
    }
    Vulkan::DescriptorTableMaterializer worker;
    for (size_t index = 0; index < queued.size(); ++index) {
        assert(std::ranges::equal(worker.Resolve(queued[index]), references[index], Equal));
        // Reuse across ticks, then revisit an older queued recipe after a newer draw.
        assert(std::ranges::equal(worker.Resolve(queued[index]), references[index], Equal));
        assert(std::ranges::equal(worker.Resolve(queued[0]), references[0], Equal));
    }
    assert(*queued.front() == *queued.front());
    assert(!(*queued.front() == *queued.back()));
    plan = {};
    {
        Vulkan::DescriptorTableWriter writer{entries, nullptr, &plan};
        writer.Reserve(256, null);
        writer.WriteRun(0, 200, different);
        writer.Write(0, null);
        writer.Write(150, null); // Out-of-order writes cannot skip this restoration.
        writer.Finish();
    }
    std::vector<VkDescriptorImageInfo> restored;
    plan.Materialize(restored);
    assert(Equal(restored[0], null) && Equal(restored[150], null));
    assert(Equal(restored[149], different) && Equal(restored[199], different));
    {
        Vulkan::DescriptorTableWriter writer{entries, nullptr, &plan};
        writer.Reserve(524288, null);
        for (size_t i = 0; i < 524288; i += 128) writer.WriteRun(i, 128, null);
        writer.Write(3, different);
        writer.Finish();
    }
    assert(plan.defaults.size() == 1 && plan.writes.size() == 1);
    plan.Materialize(restored);
    assert(restored.size() == 524288 && Equal(restored[3], different));
    for (size_t i = 0; i < restored.size(); ++i) if (i != 3) assert(Equal(restored[i], null));
    {
        Vulkan::DescriptorTableWriter writer{entries, nullptr, &plan};
        writer.Reserve(128, null);
        writer.Write(3, different);
        for (size_t i = 0; i < 4096; ++i) writer.Append(null);
        writer.Finish();
    }
    assert(plan.defaults.size() == 1 && plan.defaults[0].count == 4224);
    plan.Materialize(restored);
    assert(restored.size() == 4224 && Equal(restored[3], different) && Equal(restored.back(), null));
    run(256, 0, 128, different, false);
    different.sampler = Handle<VkSampler>(99);
    run(256, 0, 128, different, true);
    different.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    run(256, 0, 128, different, true);
}
