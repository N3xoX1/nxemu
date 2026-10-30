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
    run(256, 0, 128, different, false);
    different.sampler = Handle<VkSampler>(99);
    run(256, 0, 128, different, true);
    different.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    run(256, 0, 128, different, true);
}
