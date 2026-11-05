// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_fixture.h"
int main(int argc, char **argv) try {
    if (argc != 2)
        throw std::runtime_error("Usage: nxemu-fsr-vulkan-benchmark OUTPUT_DIRECTORY");
    const fs::path out = argv[1];
    fs::create_directories(out);
    Context vk;
    for (int i = 0; i <= 100; ++i)
        if (FsrSharpness::ToDisplay(FsrSharpness::ToStored(i)) != i)
            throw std::runtime_error("Sharpness round trip failed");
    std::ofstream info(out / "device.txt");
    info << vk.properties.deviceName << "\ndriverVersion=" << vk.properties.driverVersion
         << "\ntimestampPeriod=" << vk.properties.limits.timestampPeriod
         << "\nvalidation=" << (std::getenv("NXEMU_FSR_VALIDATION") != nullptr) << '\n';
    std::ofstream summary(out / "summary.csv"), raw(out / "timings.csv");
    summary << "case,precision,width,height,old_median_ms,new_median_ms,old_p95_ms,new_p95_ms,"
               "speedup_percent,rgb_mae,rgb_max_error,alpha_max_error,psnr_old_new_db,native_old_"
               "source_psnr_db,native_new_source_psnr_db\n";
    raw << "case,precision,frame,implementation,gpu_ms\n";
    struct Case {
        const char *name;
        int iw, ih, ow, oh;
        bool native, off;
        float l, t, r, b;
        int margin;
    };
    const Case cases[] = {
        {"720p-1080p", 1280, 720, 1920, 1080, false, false, 0, 0, 1, 1, 0},
        {"1080p-4k", 1920, 1080, 3840, 2160, false, false, 0, 0, 1, 1, 0},
        {"native-1080p", 1920, 1080, 1920, 1080, true, false, 0, 0, 1, 1, 0},
        {"native-flip", 1920, 1080, 1920, 1080, true, false, 1, 1, 0, 0, 0},
        {"native-easu-only", 1920, 1080, 1920, 1080, true, true, 0, 0, 1, 1, 0},
        {"easu-only", 1280, 720, 1920, 1080, false, true, 0, 0, 1, 1, 0},
        {"easu-crop-offset", 1280, 720, 1920, 1080, false, true, .1f, .05f, .9f, .95f, 13},
        {"easu-flip", 1280, 720, 1920, 1080, false, true, 1, 1, 0, 0, 13},
        {"rcas-flip", 1280, 720, 1920, 1080, false, false, 1, 1, 0, 0, 13},
        {"rcas-crop-offset", 1280, 720, 1920, 1080, false, false, .1f, .05f, .9f, .95f, 13}};
    for (int half = 0; half <= int(vk.fp16); ++half) {
        const char *precision = half ? "fp16" : "fp32";
        std::span<const uint32_t> fsr_vert = VULKAN_FIDELITYFX_FSR_VERT_SPV,
                                  present_vert = VULKAN_PRESENT_VERT_SPV;
        auto easu =
            vk.Pipeline(fsr_vert,
                        half ? std::span<const uint32_t>(VULKAN_FIDELITYFX_FSR_EASU_FP16_FRAG_SPV)
                             : std::span<const uint32_t>(VULKAN_FIDELITYFX_FSR_EASU_FP32_FRAG_SPV),
                        vk.pass16);
        auto rcas =
            vk.Pipeline(fsr_vert,
                        half ? std::span<const uint32_t>(VULKAN_FIDELITYFX_FSR_RCAS_FP16_FRAG_SPV)
                             : std::span<const uint32_t>(VULKAN_FIDELITYFX_FSR_RCAS_FP32_FRAG_SPV),
                        vk.pass16);
        auto copy = vk.Pipeline(present_vert, VULKAN_PRESENT_FRAG_SPV, vk.pass8);
        auto fused_easu =
            vk.Pipeline(present_vert,
                        half ? std::span<const uint32_t>(VULKAN_PRESENT_EASU_FP16_FRAG_SPV)
                             : std::span<const uint32_t>(VULKAN_PRESENT_EASU_FP32_FRAG_SPV),
                        vk.pass8);
        auto fused_rcas =
            vk.Pipeline(present_vert,
                        half ? std::span<const uint32_t>(VULKAN_PRESENT_RCAS_FP16_FRAG_SPV)
                             : std::span<const uint32_t>(VULKAN_PRESENT_RCAS_FP32_FRAG_SPV),
                        vk.pass8);
        for (const auto &c : cases) {
            const int w = c.ow + 2 * c.margin, h = c.oh + 2 * c.margin;
            auto source = vk.MakeImage(c.iw, c.ih, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
            auto a = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R16G16B16A16_SFLOAT, vk.pass16),
                 b = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R16G16B16A16_SFLOAT, vk.pass16);
            auto old = vk.MakeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8),
                 candidate = vk.MakeImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
            auto staging = vk.MakeBuffer(std::max(size_t(c.iw) * c.ih, size_t(w) * h) * 4);
            const auto pattern = Pattern(c.iw, c.ih);
            std::memcpy(staging.mapped, pattern.data(), pattern.size());
            vk.Begin();
            for (const auto *im : {&source, &a, &b, &old, &candidate})
                vk.Barrier(*im, VK_IMAGE_LAYOUT_UNDEFINED);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {uint32_t(c.iw), uint32_t(c.ih), 1};
            vkCmdCopyBufferToImage(vk.cmd, staging.buffer, source.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                   &region);
            vk.Barrier(source);
            vk.Submit();
            std::array<u32, 16> ec{};
            std::array<u32, 4> rc{};
            FSR::FsrEasuConOffset(ec.data(), ec.data() + 4, ec.data() + 8, ec.data() + 12,
                                  (c.r - c.l) * c.iw, (c.b - c.t) * c.ih, float(c.iw), float(c.ih),
                                  float(c.ow), float(c.oh), c.l * c.iw, c.t * c.ih);
            FSR::FsrRcasCon(rc.data(), .25f);
            Vulkan::PresentPushConstants pc{};
            pc.transform = Vulkan::MakeOrthographicTransform(float(w), float(h));
            auto vertices = [&](float l, float t, float r, float bottom) {
                const float x = float(c.margin), y = float(c.margin), right = x + c.ow,
                            bot = y + c.oh;
                pc.vertices = {Vulkan::ScreenRectVertex(x, y, l, t),
                               Vulkan::ScreenRectVertex(right, y, r, t),
                               Vulkan::ScreenRectVertex(x, bot, l, bottom),
                               Vulkan::ScreenRectVertex(right, bot, r, bottom)};
            };
            auto render = [&](bool fused) {
                if (fused && c.off) {
                    vertices(c.l, c.t, c.r, c.b);
                    pc.filter_constants.fill(0);
                    if (!c.native)
                        std::copy_n(ec.begin(), 12, pc.filter_constants.begin());
                    vk.Draw(candidate, source, vk.pass8, fused_easu, &pc, sizeof(pc), 4);
                } else {
                    if (!fused || !c.native)
                        vk.Draw(a, source, vk.pass16, easu, ec.data(), sizeof(ec), 3);
                    if (fused && c.native)
                        vertices(c.l, c.t, c.r, c.b);
                    else
                        vertices(0, 0, 1, 1);
                    if (fused) {
                        std::copy(rc.begin(), rc.end(), pc.filter_constants.begin());
                        vk.Draw(candidate, c.native ? source : a, vk.pass8, fused_rcas, &pc,
                                sizeof(pc), 4);
                    } else if (c.off)
                        vk.Draw(old, a, vk.pass8, copy, &pc, sizeof(pc), 4);
                    else {
                        vk.Draw(b, a, vk.pass16, rcas, rc.data(), sizeof(rc), 3);
                        vk.Draw(old, b, vk.pass8, copy, &pc, sizeof(pc), 4);
                    }
                }
            };
            for (int i = 0; i < 60; ++i) {
                vk.Begin();
                render(false);
                render(true);
                vk.Submit();
            }
            std::array<std::vector<double>, 2> samples;
            for (int i = 0; i < 240; ++i) {
                vk.Begin();
                vkCmdResetQueryPool(vk.cmd, vk.queries, 0, 4);
                for (int j = 0; j < 2; ++j) {
                    int mode = (i + j) & 1;
                    vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, vk.queries,
                                        mode * 2);
                    render(mode != 0);
                    vkCmdWriteTimestamp(vk.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, vk.queries,
                                        mode * 2 + 1);
                }
                vk.Submit();
                std::array<uint64_t, 4> times{};
                Check(vkGetQueryPoolResults(vk.device, vk.queries, 0, 4, sizeof(times),
                                            times.data(), sizeof(uint64_t),
                                            VK_QUERY_RESULT_64_BIT));
                const uint64_t mask = vk.bits == 64 ? ~uint64_t{0} : (uint64_t{1} << vk.bits) - 1;
                for (int mode = 0; mode < 2; ++mode) {
                    double ms = double((times[mode * 2 + 1] - times[mode * 2]) & mask) *
                                vk.properties.limits.timestampPeriod / 1.e6;
                    samples[mode].push_back(ms);
                    raw << c.name << ',' << precision << ',' << i << ','
                        << (mode ? "fused" : "legacy") << ',' << ms << '\n';
                }
            }
            auto download = [&](const Image &image) {
                vk.Begin();
                vk.Barrier(image);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {uint32_t(w), uint32_t(h), 1};
                vkCmdCopyImageToBuffer(vk.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer,
                                       1, &r);
                VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                vk.Submit();
                const auto *p = static_cast<const unsigned char *>(staging.mapped);
                return std::vector<unsigned char>(p, p + size_t(w) * h * 4);
            };
            auto p = download(old), n = download(candidate);
            double sum = 0, sq = 0;
            int max_rgb = 0, max_a = 0;
            for (size_t i = 0; i < p.size(); ++i) {
                int d = std::abs(int(p[i]) - int(n[i]));
                if (i % 4 == 3)
                    max_a = std::max(max_a, d);
                else {
                    sum += d;
                    sq += d * d;
                    max_rgb = std::max(max_rgb, d);
                }
            }
            double count = double(w) * h * 3, m0 = Percentile(samples[0], .5),
                   m1 = Percentile(samples[1], .5);
            double source_old = 0, source_new = 0;
            if (c.native)
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        for (int channel = 0; channel < 3; ++channel) {
                            size_t target = (size_t(y) * w + x) * 4 + channel;
                            int sx = c.l > c.r ? w - 1 - x : x, sy = c.t > c.b ? h - 1 - y : y;
                            double reference = pattern[(size_t(sy) * w + sx) * 4 + channel];
                            source_old += (p[target] - reference) * (p[target] - reference);
                            source_new += (n[target] - reference) * (n[target] - reference);
                        }
            auto psnr = [&](double error) {
                return error == 0 ? INFINITY : 10 * std::log10(255. * 255. * count / error);
            };
            summary << c.name << ',' << precision << ',' << w << ',' << h << ',' << m0 << ',' << m1
                    << ',' << Percentile(samples[0], .95) << ',' << Percentile(samples[1], .95)
                    << ',' << (1 - m1 / m0) * 100 << ',' << sum / count << ',' << max_rgb << ','
                    << max_a << ',' << psnr(sq) << ',';
            if (c.native)
                summary << psnr(source_old) << ',' << psnr(source_new);
            else
                summary << ',';
            summary << '\n';
            const auto stem = std::string(c.name) + "-" + precision;
            Save(out / (stem + "-old.ppm"), p, w, h);
            Save(out / (stem + "-new.ppm"), n, w, h);
            std::cout << stem << ": " << m0 << " -> " << m1 << " ms, max RGB " << max_rgb
                      << ", alpha " << max_a << '\n';
            if ((!c.native && max_rgb > 1) || max_a > 1)
                throw std::runtime_error("Image equivalence regression in " + stem);
            if (c.native && source_new > source_old + 0.000001)
                throw std::runtime_error("Native image fidelity regression in " + stem);
            for (auto *image : {&source, &a, &b, &old, &candidate})
                vk.Free(*image);
            vk.Free(staging);
            Check(vkResetDescriptorPool(vk.device, vk.descriptors, 0));
        }
        for (auto pipeline : {easu, rcas, copy, fused_easu, fused_rcas})
            vkDestroyPipeline(vk.device, pipeline, nullptr);
    }
    std::cout << "Validation errors: " << validation_errors << '\n';
    return validation_errors ? 1 : 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
