// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <bit>
#include "yuzu_video_core/area_sampling.h"
#include "area_bundle_reference_spv.h"
#include "vulkan_fixture.h"
#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"

// Independent texel-by-texel box integration, including extended texture edges.
std::array<double, 4> Reference(const std::vector<unsigned char> &source, int w, int h, double left,
                                double top, double right, double bottom) {
    if (left > right)
        std::swap(left, right);
    if (top > bottom)
        std::swap(top, bottom);
    std::array<double, 4> result{};
    for (int y = int(std::floor(top)); y < int(std::ceil(bottom)); ++y) {
        const double wy = std::max(0., std::min(bottom, double(y + 1)) - std::max(top, double(y)));
        for (int x = int(std::floor(left)); x < int(std::ceil(right)); ++x) {
            const double wx =
                std::max(0., std::min(right, double(x + 1)) - std::max(left, double(x)));
            size_t index = (size_t(std::clamp(y, 0, h - 1)) * w + std::clamp(x, 0, w - 1)) * 4;
            for (size_t k = 0; k < 4; ++k)
                result[k] += wx * wy * source[index + k];
        }
    }
    for (auto &value : result)
        value /= (right - left) * (bottom - top);
    return result;
}

int main(int argc, char **argv) try {
    if (argc != 2)
        throw std::runtime_error("Usage: nxemu-area-vulkan-benchmark OUTPUT_DIRECTORY");
    fs::path out(argv[1]);
    fs::create_directories(out);
    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    std::ofstream(out / "device.txt") << vk.properties.deviceName << "\nfp16=" << vk.fp16 << '\n';
    std::vector<VkPipeline> pipelines{
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, AREA_BUNDLE_REFERENCE_SPV, vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, PRESENT_AREA_FRAG_SPV, vk.pass8),
        vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_FRAG_SPV, vk.pass8)};
    std::vector<std::string> names{"bundle", "area", "bilinear"};
    struct Case {
        const char *name;
        int iw, ih, ow, oh;
        float l, t, r, b;
        int kind, margin;
    };
    const Case cases[] = {
        {"4k-1080p", 3840, 2160, 1920, 1080, 0, 0, 1, 1, 0, 0},
        {"4k-540p", 3840, 2160, 960, 540, 0, 0, 1, 1, 0, 0},
        {"4k-720p", 3840, 2160, 1280, 720, 0, 0, 1, 1, 0, 0},
        {"1440p-1080p", 2560, 1440, 1920, 1080, 0, 0, 1, 1, 0, 0},
        {"1080p-720p", 1920, 1080, 1280, 720, 0, 0, 1, 1, 0, 0},
        {"4k-1080p-phase", 3840, 2160, 1920, 1080, .00013f, .00017f, .99991f, .99989f, 0, 13},
        {"padded-crop", 1600, 900, 640, 360, 0, 0, .8f, .8f, 0, 0},
        {"flip", 1920, 1080, 1280, 720, 1, 1, 0, 0, 0, 13},
        {"native-crop", 1600, 900, 1280, 720, 0, 0, .8f, .8f, 0, 0},
        {"noise-aligned-8x", 512, 512, 64, 64, 0, 0, 1, 1, 1, 3},
        {"noise-8x", 512, 512, 64, 64, .001f, -.002f, 1.001f, .998f, 1, 3},
        {"mixed-axes", 128, 64, 32, 128, .013f, .027f, .993f, .971f, 1, 3},
        {"outside-edges", 53, 47, 27, 23, -.04f, -.03f, 1.05f, 1.07f, 1, 3},
        {"wide-source", 16384, 17, 4093, 11, .000019f, .01f, .999983f, .99f, 1, 3},
        {"tiny-output", 31, 29, 1, 1, 0, 0, 1, 1, 1, 1},
        {"constant-black", 64, 64, 19, 21, -.03f, -.01f, 1.02f, 1.04f, 2, 0},
        {"constant-white", 64, 64, 19, 21, -.03f, -.01f, 1.02f, 1.04f, 3, 0}};
    std::ofstream timings(out / "timings.csv"), errors(out / "quality.csv");
    timings << "case,mode,frame,gpu_ms\n";
    errors << "case,mode,rgba_max_error,rgba_mae,samples\n";
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
                    uint32_t noise = uint32_t(x + y * c.iw + 1) * 747796405u + 2891336453u;
                    for (size_t k = 0; k < 4; ++k) {
                        noise ^= noise >> 16;
                        noise *= 2246822519u;
                        pattern[i + k] = static_cast<unsigned char>(noise >> 24);
                    }
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
        const std::array<float, 4> area{std::abs(c.r - c.l) * c.iw / c.ow,
                                        std::abs(c.b - c.t) * c.ih / c.oh, 1.f / c.iw, 1.f / c.ih};
        for (size_t k = 0; k < 4; ++k)
            pc.filter_constants[k] = std::bit_cast<uint32_t>(area[k]);
        pc.filter_constants[4] =
            VideoCore::AreaSamplingGrid(c.iw, c.ih, c.l, c.t, c.r, c.b, c.ow, c.oh);
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
        for (size_t m = 0; m < pipelines.size(); ++m) {
            double maximum = 0, sum = 0;
            size_t checked = 0;
            const int step = std::max(1, std::min(c.ow, c.oh) / 64);
            for (int y = 0; y < c.oh; y += step)
                for (int x = 0; x < c.ow; x += step) {
                    const auto ref = Reference(
                        pattern, c.iw, c.ih, (double(c.l) + (double(c.r) - c.l) * x / c.ow) * c.iw,
                        (double(c.t) + (double(c.b) - c.t) * y / c.oh) * c.ih,
                        (double(c.l) + (double(c.r) - c.l) * (x + 1) / c.ow) * c.iw,
                        (double(c.t) + (double(c.b) - c.t) * (y + 1) / c.oh) * c.ih);
                    size_t i = (size_t(y + c.margin) * w + x + c.margin) * 4;
                    for (size_t k = 0; k < 4; ++k) {
                        double error = std::abs(ref[k] - pixels[m][i + k]);
                        maximum = std::max(maximum, error);
                        sum += error;
                        ++checked;
                    }
                }
            errors << c.name << ',' << names[m] << ',' << maximum << ',' << sum / checked << ','
                   << checked << '\n';
            Save(out / (std::string(c.name) + "-" + names[m] + ".ppm"), pixels[m], w, h);
            std::cout << c.name << ' ' << names[m] << ' ' << Percentile(samples[m], .5)
                      << " ms CPU-reference max=" << maximum << " mae=" << sum / checked << '\n';
            if (m == 1 && maximum > 2.1)
                throw std::runtime_error("Area differs from independent CPU box integral by more "
                                         "than sampler/UNORM tolerance");
            if (c.kind >= 2 && maximum > .51)
                throw std::runtime_error("Constant RGBA not preserved");
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
