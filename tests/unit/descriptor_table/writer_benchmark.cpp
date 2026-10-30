#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <type_traits>
#include "yuzu_video_core/renderer_vulkan/descriptor_table_writer.h"

template <class T> T Handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
bool Equal(const VkDescriptorImageInfo& a, const VkDescriptorImageInfo& b) {
    return a.imageView == b.imageView && a.sampler == b.sampler && a.imageLayout == b.imageLayout;
}
struct Result { double ms; unsigned changes; std::vector<VkDescriptorImageInfo> entries; };
Result Run(size_t count, bool tracked) {
    const VkDescriptorImageInfo null{VK_NULL_HANDLE, Handle<VkImageView>(1), VK_IMAGE_LAYOUT_GENERAL};
    std::vector<uintptr_t> ids(count, 1);
    for (size_t i = 3; i < count; i += 1024) ids[i] = i + 100;
    std::vector<VkDescriptorImageInfo> entries;
    Vulkan::DescriptorTableWriterState metadata;
    std::shared_ptr<const std::vector<VkDescriptorImageInfo>> snapshot;
    unsigned changes{};
    constexpr unsigned rounds = 40;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) {
        ids[3] = 103 + round / 9;
        bool changed;
        if (tracked) {
            Vulkan::DescriptorTableWriter writer{entries, &metadata};
            const size_t base = writer.Reserve(count, null);
            for (size_t i = 0; i < count;) {
                constexpr size_t block = Vulkan::DescriptorTableWriter::BLOCK_ENTRIES;
                if (i % block == 0 && i + block <= count &&
                    std::all_of(ids.begin() + i, ids.begin() + i + block,
                                [](uintptr_t id) { return id == 1; })) {
                    writer.WriteRun(base + i, block, null);
                    i += block;
                    continue;
                }
                auto entry = null;
                entry.imageView = Handle<VkImageView>(ids[i]);
                writer.Write(base + i, entry);
                ++i;
            }
            writer.Finish();
            changed = writer.Changed();
        } else {
            entries.clear();
            entries.resize(count, null);
            for (size_t i = 0; i < count; ++i) {
                if (ids[i] != 1) entries[i].imageView = Handle<VkImageView>(ids[i]);
            }
            changed = !snapshot || !std::ranges::equal(entries, *snapshot, Equal);
        }
        if (changed) {
            snapshot = std::make_shared<const std::vector<VkDescriptorImageInfo>>(entries);
            ++changes;
        }
    }
    return {std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / rounds,
            changes, std::move(entries)};
}
int main() {
    std::puts("entries,rebuild_compare_ms,tracked_write_ms,speedup,content_changes");
    for (size_t count : {4096U, 524288U}) {
        std::array<double, 5> a{}, b{};
        unsigned changes{};
        for (size_t trial = 0; trial < a.size(); ++trial) {
            Result scalar{}, tracked{};
            if (trial % 2) { tracked = Run(count, true); scalar = Run(count, false); }
            else { scalar = Run(count, false); tracked = Run(count, true); }
            if (scalar.changes != tracked.changes ||
                !std::ranges::equal(scalar.entries, tracked.entries, Equal)) return 1;
            a[trial] = scalar.ms; b[trial] = tracked.ms; changes = tracked.changes;
        }
        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
        std::printf("%zu,%.6f,%.6f,%.3f,%u\n", count, a[2], b[2], a[2] / b[2], changes);
    }
}
