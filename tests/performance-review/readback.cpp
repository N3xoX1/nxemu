// Measures the submit/wait boundary of runtime image mask readback on the real GPU.
// The batched control has no intervening host alias consumers. It is not a drop-in fix.
#define main PriorGpuMain
#include "../pr396/vulkan-synthetic.cpp"
#undef main
#include <chrono>

using u32 = uint32_t;
constexpr u32 mask_words = 257;
constexpr u32 mask_bytes = mask_words * sizeof(u32);
volatile u32 observed_mask{};

void RecordMask(Fixture& f, VkCommandBuffer cb, bool host_read) {
    Fixture::Barrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(cb, f.buffers[2].buffer, 0, mask_bytes, 0);
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = f.buffers[2].buffer; b.offset = 0; b.size = mask_bytes;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 1, &b, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, f.pipeline_layout, 0, 1, &f.set, 0, nullptr);
    vkCmdDispatch(cb, 129, 1, 1);
    if (host_read) {
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &b, 0, nullptr);
    }
}

void Verify(Fixture& f) {
    for (u32 handle : {0U, 31U, 32U, 4095U, 8191U, 8192U, 8193U}) {
        f.buffers[1].mapped[0] = handle;
        for (u32 batch : {1U, 16U}) {
            f.Begin();
            for (u32 i = 0; i < batch; ++i) RecordMask(f, f.commands[1], i + 1 == batch);
            f.Submit();
            for (u32 word = 0; word < mask_words; ++word) {
                const u32 expected = handle < 8193 && word == handle / 32 ? 1U << (handle % 32) : 0;
                if (f.buffers[2].mapped[word] != expected) throw std::runtime_error("Incorrect GPU write mask");
            }
        }
    }
    std::cout << "PASS write-mask boundary handles including invalid handle: "
              << 7 * 2 * mask_words << " words verified\n";
}

double Sample(Fixture& f, bool immediate) {
    constexpr u32 launches = 128;
    const auto start = std::chrono::steady_clock::now();
    if (immediate) {
        for (u32 i = 0; i < launches; ++i) {
            f.Begin(); RecordMask(f, f.commands[1], true); f.Submit();
            observed_mask = f.buffers[2].mapped[mask_words - 1];
        }
    } else {
        f.Begin();
        for (u32 i = 0; i < launches; ++i) RecordMask(f, f.commands[1], i + 1 == launches);
        f.Submit(); observed_mask = f.buffers[2].mapped[mask_words - 1];
    }
    const auto end = std::chrono::steady_clock::now();
    if (observed_mask != 1) throw std::runtime_error("Benchmark mask incorrect");
    return std::chrono::duration<double, std::micro>(end - start).count() / launches;
}

int main(int argc, char** argv) try {
    if (argc != 3) return 2;
    Fixture f(argv[1]);
    const VkDescriptorBufferInfo mask_info{f.buffers[2].buffer, 0, mask_bytes};
    VkWriteDescriptorSet mask_descriptor{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    mask_descriptor.dstSet = f.set; mask_descriptor.dstBinding = 1;
    mask_descriptor.descriptorCount = 1;
    mask_descriptor.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mask_descriptor.pBufferInfo = &mask_info;
    vkUpdateDescriptorSets(f.device, 1, &mask_descriptor, 0, nullptr);
    Verify(f);
    if (std::string(argv[2]) == "check") return 0;
    f.buffers[1].mapped[0] = 8192;
    std::array<std::vector<double>, 2> results;
    for (u32 run = 0; run < 25; ++run) for (u32 order = 0; order < 2; ++order) {
        const u32 mode = (run + order) % 2;
        const double time = Sample(f, mode == 1);
        if (run >= 4) results[mode].push_back(time);
    }
    for (u32 mode = 0; mode < 2; ++mode) {
        auto& times = results[mode]; std::sort(times.begin(), times.end());
        std::cout << "READBACK_BENCH mode=" << (mode ? "wait_each_dispatch" : "batch_without_host_consumers")
                  << " wall_us_per_dispatch=" << times[times.size() / 2]
                  << " min=" << times.front() << " max=" << times.back()
                  << " launches=128 samples=" << times.size() << '\n';
    }
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
