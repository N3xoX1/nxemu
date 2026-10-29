// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/texture_cache/image_info.h"

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
        depth.tile_mode.block_depth.Assign(1U);
        check(!VideoCommon::ImageInfo{depth, depth_size, samples}.rescaleable,
              "block depth restrictions are preserved");
    }
    return failures ? 1 : 0;
}
