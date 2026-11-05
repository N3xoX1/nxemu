// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint> // glslang headers require uint32_t to be declared first.

#include "catmull_reference_spv.h"
#include "vulkan_fixture.h"
#include "yuzu_video_core/host_shaders/present_catmull_rom_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_catmull_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_frag_spv.h"
#include <bit>

int main(int argc, char **argv) try {
    if (argc < 2 || argc > 4 || (argc == 4 && std::string(argv[3]) != "--report-only"))
        throw std::runtime_error("Usage: nxemu-scaling-vulkan-benchmark OUTPUT_DIRECTORY "
                                 "[CANDIDATE_SPIRV_DIRECTORY [--report-only]]");
    const fs::path out = argv[1];
    fs::create_directories(out);
    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    std::ofstream info(out / "device.txt");
    info << vk.properties.deviceName << '\n';
    auto bilinear = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_FRAG_SPV, vk.pass8);
    auto reference = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, CATMULL_REFERENCE_SPV, vk.pass8);
    auto catmull = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, PRESENT_CATMULL_ROM_FRAG_SPV, vk.pass8);
    auto cas = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_CAS_FRAG_SPV, vk.pass8);
    auto cas_catmull =
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_CAS_CATMULL_FRAG_SPV, vk.pass8);
    const auto catmull_pass16 = vk.Pass(VK_FORMAT_R16G16B16A16_UNORM);
    auto catmull16 = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, PRESENT_CATMULL_ROM_FRAG_SPV, catmull_pass16);
    struct Case {
        const char *name;
        int iw, ih, ow, oh;
        bool native;
        float l, t, r, b;
        int margin;
    };
    const Case cases[] = {{"720p-1080p", 1280, 720, 1920, 1080, false, 0, 0, 1, 1, 0},
                          {"720p-4k", 1280, 720, 3840, 2160, false, 0, 0, 1, 1, 0},
                          {"real-window", 1600, 900, 1652, 929, false, 0, 0, 1, 1, 0},
                          {"native", 1280, 720, 1280, 720, true, 0, 0, 1, 1, 0},
                          {"native-flip", 1280, 720, 1280, 720, true, 1, 1, 0, 0, 0},
                          {"crop-offset", 1280, 720, 1920, 1080, false, .1f, .05f, .9f, .95f, 13},
                          {"flip-offset", 1280, 720, 1920, 1080, false, 1, 1, 0, 0, 13},
                          {"downscale", 1280, 720, 640, 360, false, 0, 0, 1, 1, 0},
                          {"small-scale", 640, 360, 960, 540, false, 0, 0, 1, 1, 0},
                          {"medium-scale", 640, 360, 1280, 720, false, 0, 0, 1, 1, 0},
                          {"constant-black", 1, 1, 73, 41, false, 0, 0, 1, 1, 0},
                          {"constant-white", 1, 1, 73, 41, false, 0, 0, 1, 1, 0}};
    std::ofstream summary(out / "summary.csv"), timings(out / "timings.csv");
    summary << "case,rgb_max_error,rgb_mae,alpha_max_error,cas_bilinear_alpha_"
               "error,cas_catmull_"
               "alpha_error\n";
    timings << "case,mode,frame,gpu_ms\n";
    std::ofstream candidate_errors(out / "candidate-errors.csv");
    candidate_errors << "case,mode,rgb_max_error,rgb_mae,alpha_max_error\n";
    const char *modes[] = {"bilinear",       "catmull-reference16",   "catmull9",
                           "bilinear-cas50", "catmull-cas50",         "catmull5",
                           "catmull5-cas50", "catmull9-cas-two-pass", "catmull5-cas-two-pass"};
    std::vector<VkPipeline> pipelines{bilinear, reference,   catmull, cas, cas_catmull,
                                      catmull,  cas_catmull, cas,     cas};
    std::vector<const char *> mode_names(std::begin(modes), std::end(modes));
    if (argc >= 3) {
        for (const auto &name : {"catmull", "cas", "cas_catmull"}) {
            std::ifstream file(fs::path(argv[2]) / (std::string(name) + ".spv"),
                               std::ios::binary | std::ios::ate);
            if (!file)
                throw std::runtime_error("Cannot read candidate SPIR-V");
            const auto bytes = file.tellg();
            if (bytes <= 0 || bytes % 4 != 0)
                throw std::runtime_error("Invalid SPIR-V size");
            std::vector<uint32_t> code(static_cast<size_t>(bytes) / 4);
            file.seekg(0);
            if (!file.read(reinterpret_cast<char *>(code.data()),
                           static_cast<std::streamsize>(bytes)))
                throw std::runtime_error("Truncated candidate SPIR-V");
            pipelines.push_back(vk.Pipeline(VULKAN_PRESENT_VERT_SPV, code, vk.pass8));
        }
        mode_names.insert(mode_names.end(),
                          {"candidate-catmull", "candidate-cas50", "candidate-catmull-cas50"});
    }
    const int mode_count = static_cast<int>(pipelines.size());
    for (const auto &c : cases) {
        int w = c.ow + 2 * c.margin, h = c.oh + 2 * c.margin;
        auto source = vk.MakeImage(c.iw, c.ih, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
        std::vector<Image> targets(mode_count);
        for (auto &im : targets)
            im = vk.MakeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
        auto intermediate =
            vk.MakeImage(c.ow + 2, c.oh + 2, VK_FORMAT_R16G16B16A16_UNORM, catmull_pass16);
        auto staging = vk.MakeBuffer(std::max(size_t(c.iw) * c.ih, size_t(w) * h) * 4);
        auto pattern = Pattern(c.iw, c.ih);
        if (c.iw == 1) {
            const unsigned char value = std::string(c.name) == "constant-black" ? 0 : 255;
            pattern = {value, value, value, 137};
        }
        std::memcpy(staging.mapped, pattern.data(), pattern.size());
        vk.Begin();
        vk.Barrier(source, VK_IMAGE_LAYOUT_UNDEFINED);
        vk.Barrier(intermediate, VK_IMAGE_LAYOUT_UNDEFINED);
        for (const auto &im : targets)
            vk.Barrier(im, VK_IMAGE_LAYOUT_UNDEFINED);
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
        auto render = [&](int mode, float strength = .5f, bool fast_override = false) {
            pc.filter_constants[0] = std::bit_cast<uint32_t>(strength);
            pc.filter_constants[1] =
                (c.native ? 1U : 0U) |
                ((fast_override || mode == 5 || mode == 6 || mode == 8) ? 2U : 0U);
            if ((mode == 7 || mode == 8) && strength > .66f) {
                vk.Draw(targets[mode], source, vk.pass8, pipelines[mode_count > 9 ? 11 : 4],
                        &pc, sizeof(pc), 4);
            } else if (mode == 7 || mode == 8) {
                auto pre = pc;
                pre.transform = Vulkan::MakeOrthographicTransform(float(c.ow + 2), float(c.oh + 2));
                const float du = (c.r - c.l) / c.ow, dv = (c.b - c.t) / c.oh;
                pre.vertices = {
                    Vulkan::ScreenRectVertex(0, 0, c.l - du, c.t - dv),
                    Vulkan::ScreenRectVertex(float(c.ow + 2), 0, c.r + du, c.t - dv),
                    Vulkan::ScreenRectVertex(0, float(c.oh + 2), c.l - du, c.b + dv),
                    Vulkan::ScreenRectVertex(float(c.ow + 2), float(c.oh + 2), c.r + du, c.b + dv)};
                vk.Draw(intermediate, source, catmull_pass16, catmull16, &pre, sizeof(pre), 4);
                auto post = pc;
                const float u0 = 1.f / (c.ow + 2), v0 = 1.f / (c.oh + 2);
                post.vertices = {Vulkan::ScreenRectVertex(x, y, u0, v0),
                                 Vulkan::ScreenRectVertex(x + c.ow, y, 1.f - u0, v0),
                                 Vulkan::ScreenRectVertex(x, y + c.oh, u0, 1.f - v0),
                                 Vulkan::ScreenRectVertex(x + c.ow, y + c.oh, 1.f - u0, 1.f - v0)};
                post.filter_constants[1] = 4;
                vk.Draw(targets[mode], intermediate, vk.pass8,
                        pipelines[mode_count > 9 ? 11 : 4], &post, sizeof(post), 4);
            } else {
                vk.Draw(targets[mode], source, vk.pass8, pipelines[mode], &pc, sizeof(pc), 4);
            }
        };
        for (int warm = 0; warm < 30; ++warm) {
            vk.Begin();
            for (int m = 0; m < mode_count; ++m)
                render(m);
            vk.Submit();
        }
        std::vector<std::vector<double>> samples(mode_count);
        for (int i = 0; i < 160; ++i)
            for (int j = 0; j < mode_count; ++j) {
                int m = (i + j) % mode_count;
                vk.Begin();
                vkCmdResetQueryPool(vk.cmd, vk.queries, 0, 2);
                vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk.queries, 0);
                render(m);
                vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.queries, 1);
                vk.Submit();
                std::array<uint64_t, 2> t{};
                Check(vkGetQueryPoolResults(vk.device, vk.queries, 0, 2, sizeof(t), t.data(),
                                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
                const uint64_t mask = vk.bits == 64 ? ~uint64_t{0} : (uint64_t{1} << vk.bits) - 1;
                double ms =
                    double((t[1] - t[0]) & mask) * vk.properties.limits.timestampPeriod / 1e6;
                samples[m].push_back(ms);
                timings << c.name << ',' << mode_names[m] << ',' << i << ',' << ms << '\n';
            }
        auto download = [&](const Image &image) {
            vk.Begin();
            vk.Barrier(image);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {uint32_t(w), uint32_t(h), 1};
            vkCmdCopyImageToBuffer(vk.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1,
                                   &r);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
            vk.Submit();
            auto p = static_cast<unsigned char *>(staging.mapped);
            return std::vector<unsigned char>(p, p + size_t(w) * h * 4);
        };
        std::vector<std::vector<unsigned char>> pixels(mode_count);
        for (int m = 0; m < mode_count; ++m)
            pixels[m] = download(targets[m]);
        int rgb_max = 0, alpha_max = 0, cas_alpha = 0, catmull_alpha = 0;
        double sum = 0;
        for (size_t i = 0; i < pixels[1].size(); ++i) {
            int d = std::abs(int(pixels[1][i]) - int(pixels[2][i]));
            if (i % 4 == 3) {
                alpha_max = std::max(alpha_max, d);
                cas_alpha = std::max(cas_alpha, std::abs(int(pixels[0][i]) - int(pixels[3][i])));
                catmull_alpha =
                    std::max(catmull_alpha, std::abs(int(pixels[2][i]) - int(pixels[4][i])));
            } else {
                rgb_max = std::max(rgb_max, d);
                sum += d;
            }
        }
        const double mae = sum / (double(w) * h * 3);
        summary << c.name << ',' << rgb_max << ',' << mae << ',' << alpha_max << ',' << cas_alpha
                << ',' << catmull_alpha << '\n';
        if (rgb_max > 2 || alpha_max > 2 || mae > .15 || cas_alpha || catmull_alpha)
            throw std::runtime_error("Catmull/CAS image regression: " + std::string(c.name));
        if (mode_count > 9) {
            for (int m = 9; m < mode_count; ++m) {
                const auto &baseline = pixels[m - 7];
                int worst = 0, alpha = 0;
                double error_sum = 0;
                for (size_t i = 0; i < baseline.size(); ++i) {
                    int diff = std::abs(int(baseline[i]) - int(pixels[m][i]));
                    if (i % 4 == 3)
                        alpha = std::max(alpha, diff);
                    else {
                        worst = std::max(worst, diff);
                        error_sum += diff;
                    }
                }
                const double error_mean = error_sum / (double(w) * h * 3);
                candidate_errors << c.name << ',' << mode_names[m] << ',' << worst << ','
                                 << error_mean << ',' << alpha << '\n';
                std::cout << "Candidate error " << c.name << ' ' << mode_names[m]
                          << " max=" << worst << " mae=" << error_mean << " alpha=" << alpha
                          << '\n';
                if (argc != 4 && (worst > 2 || alpha > 1 || error_mean > .15))
                    throw std::runtime_error("Candidate image regression");
            }
        }
        for (int mode : {5, 7, 8}) {
            const auto &baseline = pixels[mode == 5 ? 2 : mode == 7 ? 4 : 6];
            int worst = 0, alpha = 0;
            double sum_error = 0;
            for (size_t i = 0; i < baseline.size(); ++i) {
                int d = std::abs(int(baseline[i]) - int(pixels[mode][i]));
                if (i % 4 == 3)
                    alpha = std::max(alpha, d);
                else {
                    worst = std::max(worst, d);
                    sum_error += d;
                }
            }
            double mean_error = sum_error / (double(w) * h * 3);
            candidate_errors << c.name << ',' << mode_names[mode] << ',' << worst << ','
                             << mean_error << ',' << alpha << '\n';
            std::cout << "Reconstruction error " << c.name << ' ' << mode_names[mode]
                      << " max=" << worst << " mae=" << mean_error << " alpha=" << alpha << '\n';
            if ((mode == 5 && (worst > 4 || alpha > 2 || mean_error > .15)) ||
                (mode != 5 && (worst > 3 || alpha > 1 || mean_error > .15)))
                throw std::runtime_error("Reconstruction image regression");
        }
        if (mode_count > 9) {
            for (float strength : {0.f, .01f, .25f, .66f, .67f, .75f, 1.f}) {
                for (int kind = 0; kind < 3; ++kind) {
                    int baseline_mode = kind == 0 ? 3 : kind == 1 ? 4 : 6;
                    int quad_mode = kind == 0 ? 10 : 11;
                    vk.Begin();
                    render(baseline_mode, strength);
                    render(quad_mode, strength, kind == 2);
                    vk.Submit();
                    auto a = download(targets[baseline_mode]), b = download(targets[quad_mode]);
                    for (size_t i = 0; i < a.size(); ++i)
                        if (std::abs(int(a[i]) - int(b[i])) > (i % 4 == 3 ? 0 : 1))
                            throw std::runtime_error(
                                "Quad CAS changed image during strength sweep");
                    if (kind > 0) {
                        int two_pass = kind == 1 ? 7 : 8;
                        vk.Begin();
                        render(two_pass, strength);
                        vk.Submit();
                        auto result = download(targets[two_pass]);
                        int max_error = 0, max_alpha = 0;
                        double error_sum = 0;
                        for (size_t i = 0; i < a.size(); ++i) {
                            int error = std::abs(int(a[i]) - int(result[i]));
                            if (i % 4 == 3)
                                max_alpha = std::max(max_alpha, error);
                            else {
                                max_error = std::max(max_error, error);
                                error_sum += error;
                            }
                        }
                        const double mae = error_sum / (double(w) * h * 3);
                        candidate_errors << c.name << ',' << mode_names[two_pass] << "-strength-"
                                         << strength << ',' << max_error << ',' << mae << ','
                                         << max_alpha << '\n';
                        if (max_error > 3 || max_alpha > 1 || mae > .15)
                            throw std::runtime_error(
                                "Two-pass CAS changed image during strength sweep");
                    }
                }
            }
        }
        if (mode_count > 9) {
            for (VkFilter filtering : {VK_FILTER_LINEAR, VK_FILTER_NEAREST}) {
                VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                ci.magFilter = ci.minFilter = filtering;
                ci.addressModeU = ci.addressModeV = ci.addressModeW =
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                VkSampler border_sampler;
                Check(vkCreateSampler(vk.device, &ci, nullptr, &border_sampler));
                VkDescriptorImageInfo image_info{border_sampler, source.view,
                                                 VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = source.set;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &image_info;
                vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
                for (float strength : {.01f, .66f, 1.f}) {
                    vk.Begin();
                    render(3, strength);
                    render(10, strength);
                    vk.Submit();
                    auto baseline = download(targets[3]), quad = download(targets[10]);
                    for (size_t i = 0; i < baseline.size(); ++i)
                        if (std::abs(int(baseline[i]) - int(quad[i])) > (i % 4 == 3 ? 0 : 1))
                            throw std::runtime_error("Border/nearest quad CAS image mismatch");
                }
                // Restore the sampler before the next comparison and before destruction.
                image_info.sampler = vk.sampler;
                vkUpdateDescriptorSets(vk.device, 1, &write, 0, nullptr);
                vkDestroySampler(vk.device, border_sampler, nullptr);
            }
        }
        // CAS=0 must preserve the selected base reconstruction, including alpha.
        vk.Begin();
        render(3, 0);
        render(4, 0);
        vk.Submit();
        for (int m = 3; m < 5; ++m) {
            auto off = download(targets[m]);
            const auto &base = pixels[m == 3 ? 0 : 2];
            for (size_t i = 0; i < off.size(); ++i)
                if (std::abs(int(off[i]) - int(base[i])) > 1)
                    throw std::runtime_error("CAS zero changed base filter");
        }
        for (int m = 0; m < mode_count; ++m) {
            Save(out / (std::string(c.name) + "-" + mode_names[m] + ".ppm"), pixels[m], w, h);
            std::cout << c.name << ' ' << mode_names[m] << ' ' << Percentile(samples[m], .5)
                      << " ms\n";
        }
        for (auto &im : targets)
            vk.Free(im);
        vk.Free(intermediate);
        vk.Free(source);
        vk.Free(staging);
        Check(vkResetDescriptorPool(vk.device, vk.descriptors, 0));
    }
    std::sort(pipelines.begin(), pipelines.end());
    pipelines.erase(std::unique(pipelines.begin(), pipelines.end()), pipelines.end());
    for (auto pipeline : pipelines)
        vkDestroyPipeline(vk.device, pipeline, nullptr);
    vkDestroyPipeline(vk.device, catmull16, nullptr);
    vkDestroyRenderPass(vk.device, catmull_pass16, nullptr);
    std::cout << "Validation errors: " << validation_errors << '\n';
    return validation_errors ? 1 : 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
