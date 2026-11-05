// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_fixture.h"
#include "yuzu_video_core/area_sampling.h"
#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_frag_spv.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/present_sharpening.h"
#include <bit>

void Require(bool condition, const char * message)
{
    if (!condition) throw std::runtime_error(message);
}

// Independent scalar resamplers: integrate each texel for Area, or interpolate
// four source texels for Bilinear. Quantize only at the reference image upload.
std::vector<uint16_t> Resize(const std::vector<unsigned char> & p, int iw, int ih,
                             int ow, int oh, float l, float t, float r, float b, bool area)
{
    std::vector<uint16_t> result(size_t(ow) * oh * 4);
    auto sample = [&](int x, int y, int k) {
        return double(p[(size_t(std::clamp(y, 0, ih - 1)) * iw + std::clamp(x, 0, iw - 1)) * 4 + k]);
    };
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x)
        {
            const double cx = (l + (double(r) - l) * (x + .5) / ow) * iw;
            const double cy = (t + (double(b) - t) * (y + .5) / oh) * ih;
            const double fw = std::abs(double(r) - l) * iw / ow;
            const double fh = std::abs(double(b) - t) * ih / oh;
            for (int k = 0; k < 4; ++k)
            {
                double value = 0;
                if (area)
                {
                    const double left = cx - fw * .5, right = cx + fw * .5;
                    const double top = cy - fh * .5, bottom = cy + fh * .5;
                    for (int sy = int(std::floor(top)); sy < int(std::ceil(bottom)); ++sy)
                        for (int sx = int(std::floor(left)); sx < int(std::ceil(right)); ++sx)
                        {
                            const double wx = std::max(0., std::min(right, double(sx + 1)) - std::max(left, double(sx)));
                            const double wy = std::max(0., std::min(bottom, double(sy + 1)) - std::max(top, double(sy)));
                            value += wx * wy * sample(sx, sy, k) / (fw * fh);
                        }
                }
                else
                {
                    const int sx = int(std::floor(cx - .5)), sy = int(std::floor(cy - .5));
                    const double ax = cx - .5 - sx, ay = cy - .5 - sy;
                    value = (1 - ay) * ((1 - ax) * sample(sx, sy, k) + ax * sample(sx + 1, sy, k)) +
                            ay * ((1 - ax) * sample(sx, sy + 1, k) + ax * sample(sx + 1, sy + 1, k));
                }
                result[(size_t(y) * ow + x) * 4 + k] = static_cast<uint16_t>(std::clamp(std::lround(value * 257), 0L, 65535L));
            }
        }
    return result;
}

int main()
try
{
    const Common::Rectangle<float> full{0, 0, 1, 1};
    for (const auto filter : {ScalingFilter::NearestNeighbor, ScalingFilter::Bilinear,
                              ScalingFilter::CatmullRom, ScalingFilter::CatmullRomFast, ScalingFilter::ScaleForce,
                              ScalingFilter::Fsr, ScalingFilter::FsrEasu})
    {
        for (bool area : {false, true})
        {
            const auto native = ResolvePresentScale(filter, area, 640, 360, full, 640, 360);
            Require(native.mode == PresentScaleMode::Native && native.filter == ScalingFilter::Bilinear,
                    "Native selected a resize filter");
            const auto down = ResolvePresentScale(filter, area, 1280, 720, full, 640, 360);
            Require(down.mode == PresentScaleMode::Downscale &&
                        down.filter == (area ? ScalingFilter::Area : ScalingFilter::Bilinear),
                    "Downscale selected an upscaler");
            Require(ResolvePresentScale(filter, area, 320, 180, full, 640, 360).filter == filter,
                    "Upscaler changed");
        }
        Require(GetPresentSharpening(filter, 0, 200) == PresentSharpening::None, "Off runs sharpening");
        Require(GetPresentSharpening(filter, 60, 50) ==
                    (filter == ScalingFilter::Fsr ? PresentSharpening::Rcas : filter == ScalingFilter::FsrEasu ? PresentSharpening::None
                                                                                                               : PresentSharpening::Cas),
                "Wrong sharpening family or stacked CAS/RCAS");
    }

    Context vk(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const auto pass = vk.Pass(VK_FORMAT_R16G16B16A16_UNORM);
    const auto area_pipeline = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, PRESENT_AREA_FRAG_SPV, pass);
    const auto linear_pipeline = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_FRAG_SPV, pass);
    const auto cas = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_CAS_FRAG_SPV, vk.pass8);
    const auto rcas = vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_RCAS_FP32_FRAG_SPV, vk.pass8);
    const auto rcas16 = vk.fp16
                            ? vk.Pipeline(VULKAN_PRESENT_VERT_SPV, VULKAN_PRESENT_RCAS_FP16_FRAG_SPV, vk.pass8)
                            : VK_NULL_HANDLE;
    struct Case
    {
        const char * name;
        int iw, ih, ow, oh;
        float l, t, r, b;
    };
    const Case cases[] = {
        {"2x", 256, 144, 128, 72, 0, 0, 1, 1},
        {"3x", 384, 216, 128, 72, 0, 0, 1, 1},
        {"crop", 320, 180, 128, 72, .1f, .1f, .9f, .9f},
        {"flip", 320, 180, 128, 72, .9f, .9f, .1f, .1f},
        {"native", 128, 72, 128, 72, 0, 0, 1, 1},
        {"native-crop", 160, 90, 128, 72, 0, 0, .8f, .8f},
        {"native-half-texel", 256, 128, 128, 64, 1.f / 512, 1.f / 256, .5f + 1.f / 512, .5f + 1.f / 256},
        {"mixed", 128, 64, 64, 128, 0, 0, 1, 1}};
    auto constants = [](int w, int h, float l, float t, float r, float b) {
        Vulkan::PresentPushConstants pc{};
        pc.transform = Vulkan::MakeOrthographicTransform(float(w), float(h));
        pc.vertices = {Vulkan::ScreenRectVertex(0, 0, l, t), Vulkan::ScreenRectVertex(float(w), 0, r, t),
                       Vulkan::ScreenRectVertex(0, float(h), l, b), Vulkan::ScreenRectVertex(float(w), float(h), r, b)};
        return pc;
    };
    for (const auto & c : cases)
        for (bool area : {false, true})
        {
            const auto selection = ResolvePresentScale(ScalingFilter::Fsr, area, c.iw, c.ih,
                                                       Common::Rectangle<float>{c.l, c.t, c.r, c.b}, c.ow, c.oh);
            const bool native = selection.mode == PresentScaleMode::Native;
            const bool aligned = IsPresentCropTexelAligned(c.iw, c.ih,
                                                           Common::Rectangle<float>{c.l, c.t, c.r, c.b});
            auto source = vk.MakeImage(c.iw, c.ih, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
            auto intermediate = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R16G16B16A16_UNORM, pass);
            auto reference = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R16G16B16A16_UNORM, pass);
            auto actual = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
            auto expected = vk.MakeImage(c.ow, c.oh, VK_FORMAT_R8G8B8A8_UNORM, vk.pass8);
            auto staging = vk.MakeBuffer(std::max(size_t(c.iw) * c.ih * 4, size_t(c.ow) * c.oh * 8));
            auto pattern = Pattern(c.iw, c.ih);
            const auto cpu = Resize(pattern, c.iw, c.ih, c.ow, c.oh, c.l, c.t, c.r, c.b, area && !native);
            auto upload = [&](const Image & image, const auto & bytes) {
                std::memcpy(staging.mapped, bytes.data(), bytes.size() * sizeof(bytes[0]));
                vk.Begin();
                vk.Barrier(image, VK_IMAGE_LAYOUT_UNDEFINED);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {uint32_t(image.w), uint32_t(image.h), 1};
                vkCmdCopyBufferToImage(vk.cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                vk.Barrier(image);
                vk.Submit();
            };
            upload(source, pattern);
            upload(reference, cpu);
            vk.Begin();
            for (const auto * image : {&intermediate, &actual, &expected})
                vk.Barrier(*image, VK_IMAGE_LAYOUT_UNDEFINED);
            auto resize = constants(c.ow, c.oh, c.l, c.t, c.r, c.b);
            if (area && !native)
            {
                const std::array<float, 4> values{std::abs(c.r - c.l) * c.iw / c.ow, std::abs(c.b - c.t) * c.ih / c.oh, 1.f / c.iw, 1.f / c.ih};
                for (size_t i = 0; i < 4; ++i)
                    resize.filter_constants[i] = std::bit_cast<u32>(values[i]);
                resize.filter_constants[4] = VideoCore::AreaSamplingGrid(c.iw, c.ih, c.l, c.t, c.r, c.b, c.ow, c.oh);
            }
            if (!native || !aligned) vk.Draw(intermediate, source, pass, area && !native ? area_pipeline : linear_pipeline, &resize, sizeof(resize), 4);
            vk.Submit();
            for (int mode = 0; mode < 2 + int(vk.fp16); ++mode)
            {
                const bool use_rcas = mode != 0;
                const auto pipeline = mode == 2 ? rcas16 : use_rcas ? rcas
                                                                    : cas;
                auto sharp = constants(c.ow, c.oh, 0, 0, 1, 1);
                if (use_rcas)
                    FSR::FsrRcasCon(sharp.filter_constants.data(), .5f);
                else
                    sharp.filter_constants[0] = std::bit_cast<u32>(.6f);
                const bool direct_source = native ? (!use_rcas || aligned) : (!area && !use_rcas);
                auto direct = direct_source
                                  ? constants(c.ow, c.oh, c.l, c.t, c.r, c.b)
                                  : sharp;
                direct.filter_constants = sharp.filter_constants;
                vk.Begin();
                vk.Draw(expected, reference, vk.pass8, pipeline, &sharp, sizeof(sharp), 4);
                vk.Draw(actual, direct_source ? source : intermediate,
                        vk.pass8, pipeline, &direct, sizeof(direct), 4);
                vk.Submit();
                auto download = [&](const Image & image) {
                    vk.Begin();
                    vk.Barrier(image);
                    VkBufferImageCopy copy{};
                    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    copy.imageExtent = {uint32_t(c.ow), uint32_t(c.oh), 1};
                    vkCmdCopyImageToBuffer(vk.cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, staging.buffer, 1, &copy);
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                    vk.Submit();
                    const auto * p = static_cast<const unsigned char *>(staging.mapped);
                    return std::vector<unsigned char>(p, p + size_t(c.ow) * c.oh * 4);
                };
                const auto a = download(actual), e = download(expected);
                int maximum = 0, alpha = 0, changed = 0;
                // Direct cropped/fused paths may read outside the useful crop at its
                // edge; compare the interior where both neighborhoods are identical.
                for (int y = 1; y < c.oh - 1; ++y)
                    for (int x = 1; x < c.ow - 1; ++x)
                        for (int k = 0; k < 4; ++k)
                        {
                            size_t i = (size_t(y) * c.ow + x) * 4 + k;
                            (k == 3 ? alpha : maximum) = std::max(k == 3 ? alpha : maximum, std::abs(int(a[i]) - e[i]));
                            if (k < 3 && std::abs(int(a[i]) - int((cpu[i] + 128) / 257)) > 1) ++changed;
                        }
                std::cout << c.name << ' ' << (area ? "area" : "bilinear") << ' ' << (mode == 2 ? "rcas-fp16" : use_rcas ? "rcas-fp32"
                                                                                                                         : "cas")
                          << " max=" << maximum << " alpha=" << alpha << " changed=" << changed << '\n';
                Require(maximum <= 2 && alpha <= 1, "Sharpening differs from independently resized image");
                Require(changed > 0, "Sharpening was bypassed");
            }
            for (auto * image : {&source, &intermediate, &reference, &actual, &expected})
                vk.Free(*image);
            vk.Free(staging);
            Check(vkResetDescriptorPool(vk.device, vk.descriptors, 0));
        }
    for (auto pipeline : {area_pipeline, linear_pipeline, cas, rcas})
        vkDestroyPipeline(vk.device, pipeline, nullptr);
    if (rcas16) vkDestroyPipeline(vk.device, rcas16, nullptr);
    vkDestroyRenderPass(vk.device, pass, nullptr);
    Require(!validation_errors, "Vulkan validation errors");
    return 0;
}
catch (const std::exception & e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
