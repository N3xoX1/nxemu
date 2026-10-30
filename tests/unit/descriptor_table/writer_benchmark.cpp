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
VkDescriptorImageInfo RuntimeSampler(size_t index) {
    return {Handle<VkSampler>(index < 20 ? index + 10 : 1), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
}
struct Result { double ms; unsigned changes; std::vector<VkDescriptorImageInfo> entries; double worker_ms; };
Result Run(size_t count, unsigned mode) {
    const VkDescriptorImageInfo null{VK_NULL_HANDLE, Handle<VkImageView>(1), VK_IMAGE_LAYOUT_GENERAL};
    std::vector<uintptr_t> ids(count, 1);
    for (size_t i = 3; i < count; i += 1024) ids[i] = i + 100;
    std::vector<VkDescriptorImageInfo> entries;
    Vulkan::DescriptorTableWriterState metadata;
    std::shared_ptr<const std::vector<VkDescriptorImageInfo>> snapshot;
    Vulkan::DescriptorTablePlan plan;
    std::shared_ptr<const Vulkan::DescriptorTablePlan> plan_snapshot;
    Vulkan::DescriptorTableMaterializer worker;
    double worker_ms{};
    unsigned changes{};
    constexpr unsigned rounds = 40;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round) {
        ids[3] = 103 + round / 9;
        bool changed;
        if (mode) {
            Vulkan::DescriptorTableWriter writer{entries, &metadata, mode == 2 ? &plan : nullptr};
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
            for (size_t index = 0; index < 4096; ++index) writer.Append(RuntimeSampler(index));
            writer.Finish();
            changed = mode == 2 ? !plan_snapshot || !(plan == *plan_snapshot) : writer.Changed();
        } else {
            entries.clear();
            entries.resize(count, null);
            for (size_t i = 0; i < count; ++i) {
                if (ids[i] != 1) entries[i].imageView = Handle<VkImageView>(ids[i]);
            }
            for (size_t index = 0; index < 4096; ++index) entries.push_back(RuntimeSampler(index));
            changed = !snapshot || !std::ranges::equal(entries, *snapshot, Equal);
        }
        if (changed) {
            if (mode == 2) plan_snapshot = std::make_shared<const Vulkan::DescriptorTablePlan>(plan);
            else snapshot = std::make_shared<const std::vector<VkDescriptorImageInfo>>(entries);
            ++changes;
        }
        if (mode == 2) {
            const auto work_start = std::chrono::steady_clock::now();
            worker.Resolve(plan_snapshot);
            worker_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - work_start).count();
        }
    }
    const double total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (mode == 2) entries = worker.Resolve(plan_snapshot);
    return {(total_ms - worker_ms) / rounds, changes, std::move(entries), worker_ms / rounds};
}
int main() {
    std::puts("entries,rebuild_compare_ms,tracked_write_ms,compact_producer_ms,compact_worker_ms,content_changes");
    for (size_t count : {4096U, 524288U}) {
        std::array<double, 5> a{}, b{}, c{}, d{};
        unsigned changes{};
        for (size_t trial = 0; trial < a.size(); ++trial) {
            Result scalar{}, tracked{}, compact{};
            if (trial % 2) { compact = Run(count, 2); tracked = Run(count, 1); scalar = Run(count, 0); }
            else { scalar = Run(count, 0); tracked = Run(count, 1); compact = Run(count, 2); }
            if (scalar.changes != tracked.changes || scalar.changes != compact.changes ||
                !std::ranges::equal(scalar.entries, tracked.entries, Equal) ||
                !std::ranges::equal(scalar.entries, compact.entries, Equal)) return 1;
            a[trial] = scalar.ms; b[trial] = tracked.ms; changes = tracked.changes;
            c[trial] = compact.ms; d[trial] = compact.worker_ms;
        }
        std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
        std::sort(c.begin(), c.end()); std::sort(d.begin(), d.end());
        std::printf("%zu,%.6f,%.6f,%.6f,%.6f,%u\n", count, a[2], b[2], c[2], d[2], changes);
    }
}
