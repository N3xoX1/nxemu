// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include "scaleforce_reference_spv.h"
#include "vulkan_fixture.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fast_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_scaleforce_fast_fp32_frag_spv.h"

int main(int argc, char **argv) try {
    if (argc != 2)
        throw std::runtime_error("Usage: nxemu-scaleforce-vulkan-benchmark OUTPUT_DIRECTORY");
    fs::path out(argv[1]);
    fs::create_directories(out);
    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    std::ofstream(out / "device.txt") << vk.properties.deviceName << "\nfp16=" << vk.fp16 << '\n';
    std::vector<VkPipeline> pipelines{
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, SCALEFORCE_REFERENCE_SPV, vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_SCALEFORCE_FP32_FRAG_SPV, vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_SCALEFORCE_FAST_FP32_FRAG_SPV,
                    vk.pass8)};
    std::vector<std::string> names{"reference32", "classic32", "fast32"};
    if (vk.fp16) {
        pipelines.push_back(vk.Pipeline(VULKAN_PRESENT_VERT_SPV,
                                        VULKAN_PRESENT_SCALEFORCE_FP16_FRAG_SPV, vk.pass8));
        pipelines.push_back(vk.Pipeline(VULKAN_PRESENT_VERT_SPV,
                                        VULKAN_PRESENT_SCALEFORCE_FAST_FP16_FRAG_SPV, vk.pass8));
        names.insert(names.end(), {"classic16", "fast16"});
    }
    struct Case {
        const char *name;
        int iw, ih, ow, oh;
        float l, t, r, b;
        int kind, margin;
    };
    const Case cases[] = {{"720p-1080p", 1280, 720, 1920, 1080, 0, 0, 1, 1, 0, 0},
                          {"720p-4k", 1280, 720, 3840, 2160, 0, 0, 1, 1, 0, 0},
                          {"mario-window", 1600, 900, 1652, 929, 0, 0, .8f, .8f, 0, 0},
                          {"native", 1280, 720, 1280, 720, 0, 0, 1, 1, 0, 0},
                          {"flip", 1280, 720, 1920, 1080, 1, 1, 0, 0, 0, 13},
                          {"crop", 1280, 720, 1920, 1080, .1f, .05f, .9f, .95f, 0, 13},
                          {"downscale", 1280, 720, 640, 360, 0, 0, 1, 1, 0, 0},
                          {"pixel-art", 32, 32, 1024, 1024, 0, 0, 1, 1, 1, 0},
                          {"constant-black", 64, 64, 128, 128, .25f, .25f, .75f, .75f, 2, 0},
                          {"constant-white", 64, 64, 128, 128, .25f, .25f, .75f, .75f, 3, 0}};
    std::ofstream timings(out / "timings.csv"), errors(out / "quality.csv");
    timings << "case,mode,frame,gpu_ms\n";
    errors
        << "case,mode,rgb_max_error,rgb_mae,psnr_db,pixels_changed_gt2_percent,alpha_max_error\n";
    for (const auto &c : cases) {
        int w = c.ow + 2 * c.margin, h = c.oh + 2 * c.margin;
        auto source = vk.MakeImage(c.iw, c.ih, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
        std::vector<Image> targets(pipelines.size());
        for (auto &t : targets)
            t = vk.MakeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
        auto staging = vk.MakeBuffer(std::max(size_t(w) * h, size_t(c.iw) * c.ih) * 4);
        auto pattern = Pattern(c.iw, c.ih);
        for (int y = 0; y < c.ih; y++)
            for (int x = 0; x < c.iw; x++) {
                size_t i = (size_t(y) * c.iw + x) * 4;
                if (c.kind == 1) {
                    bool edge = x >= y && x <= y + 3;
                    pattern[i] = edge ? 255 : ((x / 4 + y / 4) % 2 ? 210 : 15);
                    pattern[i + 1] = edge ? 30 : pattern[i];
                    pattern[i + 2] = edge ? 90 : pattern[i];
                    pattern[i + 3] = 255;
                } else if (c.kind >= 2) {
                    pattern[i] = pattern[i + 1] = pattern[i + 2] = c.kind == 2 ? 0 : 255;
                    pattern[i + 3] = 137;
                }
            }
        std::memcpy(staging.mapped, pattern.data(), pattern.size());
        vk.Begin();
        vk.Barrier(source, VK_IMAGE_LAYOUT_UNDEFINED);
        for (const auto &t : targets)
            vk.Barrier(t, VK_IMAGE_LAYOUT_UNDEFINED);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {uint32_t(c.iw), uint32_t(c.ih), 1};
        vkCmdCopyBufferToImage(vk.cmd, staging.buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
        vk.Barrier(source);
        vk.Submit();
        Vulkan::PresentPushConstants pc{};
        pc.transform = Vulkan::MakeOrthographicTransform(float(w), float(h));
        float x = float(c.margin), y = x;
        pc.vertices = {Vulkan::ScreenRectVertex(x, y, c.l, c.t),
                       Vulkan::ScreenRectVertex(x + c.ow, y, c.r, c.t),
                       Vulkan::ScreenRectVertex(x, y + c.oh, c.l, c.b),
                       Vulkan::ScreenRectVertex(x + c.ow, y + c.oh, c.r, c.b)};
        auto render = [&](size_t m) {
            vk.Draw(targets[m], source, vk.pass8, pipelines[m], &pc, sizeof(pc), 4);
        };
        for (int warm = 0; warm < 30; warm++) {
            vk.Begin();
            for (size_t m = 0; m < pipelines.size(); m++)
                render(m);
            vk.Submit();
        }
        std::vector<std::vector<double>> samples(pipelines.size());
        for (int i = 0; i < 160; i++)
            for (size_t j = 0; j < pipelines.size(); j++) {
                size_t m = (i + j) % pipelines.size();
                vk.Begin();
                vkCmdResetQueryPool(vk.cmd, vk.queries, 0, 2);
                vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk.queries, 0);
                render(m);
                vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.queries, 1);
                vk.Submit();
                std::array<uint64_t, 2> t{};
                Check(vkGetQueryPoolResults(vk.device, vk.queries, 0, 2, sizeof(t), t.data(),
                                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
                uint64_t mask = vk.bits == 64 ? ~uint64_t{0} : (uint64_t{1} << vk.bits) - 1;
                double ms =
                    double((t[1] - t[0]) & mask) * vk.properties.limits.timestampPeriod / 1e6;
                samples[m].push_back(ms);
                timings << c.name << ',' << names[m] << ',' << i << ',' << ms << '\n';
            }
        auto download = [&](const Image &im) {
            vk.Begin();
            vk.Barrier(im);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {uint32_t(w), uint32_t(h), 1};
            vkCmdCopyImageToBuffer(vk.cmd, im.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1,
                                   &r);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
            vk.Submit();
            const auto *p = static_cast<const unsigned char *>(staging.mapped);
            return std::vector<unsigned char>(p, p + size_t(w) * h * 4);
        };
        std::vector<std::vector<unsigned char>> pixels;
        for (size_t m = 0; m < pipelines.size(); m++)
            pixels.push_back(download(targets[m]));
        if (pixels[0] != pixels[1])
            throw std::runtime_error("Classic FP32 changed relative to frozen reference");
        for (size_t m = 0; m < pipelines.size(); m++) {
            const auto &base = pixels[m >= 3 ? 3 : 1];
            int maximum = 0, alpha = 0;
            double sum = 0, squared = 0;
            size_t changed = 0;
            for (size_t i = 0; i < base.size(); i += 4) {
                int pixel_max = 0;
                for (size_t k = 0; k < 3; k++) {
                    int d = std::abs(int(base[i + k]) - int(pixels[m][i + k]));
                    maximum = std::max(maximum, d);
                    pixel_max = std::max(pixel_max, d);
                    sum += d;
                    squared += d * d;
                }
                changed += pixel_max > 2;
                alpha = std::max(alpha, std::abs(int(base[i + 3]) - int(pixels[m][i + 3])));
            }
            double mae = sum / (double(w) * h * 3), mse = squared / (double(w) * h * 3);
            double psnr = mse > 0 ? 10 * std::log10(255. * 255. / mse) : 999.;
            errors << c.name << ',' << names[m] << ',' << maximum << ',' << mae << ',' << psnr
                   << ',' << 100. * changed / (double(w) * h) << ',' << alpha << '\n';
            if (c.kind >= 2 && (maximum || alpha))
                throw std::runtime_error("Fast ScaleForce changed a constant color/alpha");
            Save(out / (std::string(c.name) + "-" + names[m] + ".ppm"), pixels[m], w, h);
            std::cout << c.name << ' ' << names[m] << ' ' << Percentile(samples[m], .5)
                      << " ms max=" << maximum << " mae=" << mae << '\n';
        }
        for (auto &t : targets)
            vk.Free(t);
        vk.Free(source);
        vk.Free(staging);
        Check(vkResetDescriptorPool(vk.device, vk.descriptors, 0));
    }
    for (auto p : pipelines)
        vkDestroyPipeline(vk.device, p, nullptr);
    std::cout << "Validation errors: " << validation_errors << '\n';
    return validation_errors ? 1 : 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
