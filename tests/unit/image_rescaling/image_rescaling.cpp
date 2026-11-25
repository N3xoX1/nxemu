// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/texture_cache/image_info.h"
#include "yuzu_video_core/texture_cache/util.h"

VideoSettings videoSettings{};

int main() {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    constexpr auto samples = Tegra::Texture::MsaaMode::Msaa1x1;
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    for (u32 height : {230U, 288U, 289U, 720U}) {
        Regs::RenderTargetConfig color{};
        color.format = Tegra::RenderTargetFormat::A8B8G8R8_UNORM;
        color.width = 240;
        color.height = height;
        color.depth.Assign(1U);
        Regs::Zeta depth{};
        depth.format = Tegra::DepthFormat::Z24_UNORM_S8_UINT;
        Regs::ZetaSize depth_size{};
        depth_size.width = color.width;
        depth_size.height = height;
        depth_size.depth.Assign(1U);
        const VideoCommon::ImageInfo color_info{color, samples};
        const VideoCommon::ImageInfo depth_info{depth, depth_size, samples};
        check(color_info.rescaleable == depth_info.rescaleable,
              "color and depth prepass use the same resolution eligibility");
        check(depth_info.rescaleable == (height > 288),
              "small portraits stay native, larger targets remain rescalable");
        check(color_info.downscaleable == depth_info.downscaleable,
              "color and depth use the same downscale threshold");
        Tegra::Texture::TICEntry texture{};
        texture.header_version.Assign(Tegra::Texture::TICHeaderVersion::BlockLinear);
        texture.texture_type.Assign(Tegra::Texture::TextureType::Texture2D);
        texture.format.Assign(Tegra::Texture::TextureFormat::Z24S8);
        texture.r_type.Assign(Tegra::Texture::ComponentType::UINT);
        texture.g_type.Assign(Tegra::Texture::ComponentType::UNORM);
        texture.b_type.Assign(Tegra::Texture::ComponentType::UNORM);
        texture.a_type.Assign(Tegra::Texture::ComponentType::UNORM);
        texture.width_minus_one.Assign(color.width - 1);
        texture.height_minus_1.Assign(height - 1);
        check(VideoCommon::ImageInfo{texture}.rescaleable == depth_info.rescaleable,
              "sampling the depth image does not reintroduce a different scale");
        texture.max_mip_level.Assign(5U);
        check(VideoCommon::ImageInfo{texture}.rescaleable == depth_info.rescaleable,
              "a mip chain, like a depth pyramid, scales like its base level");
        depth.tile_mode.block_depth.Assign(1U);
        check(!VideoCommon::ImageInfo{depth, depth_size, samples}.rescaleable,
              "block depth restrictions are preserved");
    }
    Tegra::Texture::TICEntry pyramid{};
    pyramid.header_version.Assign(Tegra::Texture::TICHeaderVersion::BlockLinear);
    pyramid.texture_type.Assign(Tegra::Texture::TextureType::Texture2D);
    pyramid.format.Assign(Tegra::Texture::TextureFormat::Z24S8);
    pyramid.r_type.Assign(Tegra::Texture::ComponentType::UINT);
    pyramid.g_type.Assign(Tegra::Texture::ComponentType::UNORM);
    pyramid.b_type.Assign(Tegra::Texture::ComponentType::UNORM);
    pyramid.a_type.Assign(Tegra::Texture::ComponentType::UNORM);
    pyramid.width_minus_one.Assign(1919U);
    pyramid.height_minus_1.Assign(1079U);
    pyramid.max_mip_level.Assign(10U);
    const VideoCommon::ImageInfo pyramid_info{pyramid};
    const Settings::ResolutionScalingInfo half{.up_scale = 1, .down_shift = 1, .active = true,
                                               .downscale = true};
    const Settings::ResolutionScalingInfo twice{.up_scale = 2, .active = true};
    check(pyramid_info.resources.levels == 11, "a 1920x1080 pyramid has 11 levels");
    check(VideoCommon::ScaledImageInfo(pyramid_info, half).resources.levels == 10,
          "a 960x540 pyramid only has room for 10 levels");
    check(VideoCommon::ScaledImageInfo(pyramid_info, twice).resources.levels == 11,
          "upscaling keeps every level");
    check(VideoCommon::ScaledLevelsMatch(pyramid_info, half),
          "halving maps every level it keeps onto a level of the same size");
    check(!VideoCommon::ScaledLevelsMatch(pyramid_info, twice),
          "at 2x, level 4 is 135 rows high but its 67 guest rows scale to 134");
    pyramid.max_mip_level.Assign(0U);
    check(VideoCommon::ScaledLevelsMatch(VideoCommon::ImageInfo{pyramid}, twice),
          "an image without mips always scales exactly");
    const VideoCommon::SubresourceRange whole_chain{.base = {.level = 0, .layer = 0},
                                                    .extent = {.levels = 11, .layers = 1}};
    check(VideoCommon::ClampLevels(whole_chain, 10).extent.levels == 10,
          "a view of the whole chain stops at its last scaled level");
    return failures ? 1 : 0;
}
