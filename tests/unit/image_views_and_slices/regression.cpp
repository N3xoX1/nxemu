// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <string_view>
#include "nxemu-video/video_settings.h"
#include "yuzu_video_core/texture_cache/image_info.h"
#include "yuzu_video_core/texture_cache/image_view_info.h"
#include "yuzu_video_core/texture_cache/util.h"

VideoSettings videoSettings{};
unsigned failures{};
void Check(bool ok, const char* label) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
void Views() {
    using namespace VideoCommon;
    using Tegra::Texture::TextureType;
    // Production descriptors captured from MHR: six 2D face views, depth field 6.
    for (u32 layer = 0; layer < 6; ++layer) {
        TICEntry tic{};
        tic.raw = {0x09f9000078d7ffa1ULL, 0x4007002000600015ULL,
                   0x800500ffe88000ffULL, 0x0000000003000001ULL};
        tic.address_low = 0x09f90000 + layer * 0x56000;
        tic.layer_base_0_2.Assign(layer);
        for (auto type : {TextureType::Texture2D, TextureType::Texture2DNoMipmap}) {
            tic.texture_type.Assign(type);
            const ImageInfo image{tic};
            const ImageViewInfo view{tic, static_cast<s32>(layer)};
            Check(image.type == ImageType::e2D && image.size.depth == 1,
                  "2D view does not turn into a volume");
            Check(image.size.width == 256 && image.size.height == 256,
                  "captured face dimensions are preserved");
            Check(image.resources.layers == layer + 1, "existing backing-layer calculation is preserved");
            Check(view.type == ImageViewType::e2D && view.range.base.layer == layer &&
                  view.range.extent.layers == 1, "each view exposes exactly its selected face");
        }
        tic.texture_type.Assign(TextureType::Texture2DArray);
        Check(ImageViewInfo{tic, 0}.range.extent.layers == 6, "array views retain their depth count");
        tic.texture_type.Assign(TextureType::Texture3D);
        tic.layer_base_0_2.Assign(0);
        Check(ImageInfo{tic}.size.depth == 6, "3D textures retain their depth");
    }
}
void Copies() {
    using namespace VideoCommon;
    ImageInfo dst{};
    dst.type = ImageType::e3D;
    dst.format = PixelFormat::R8_UNORM;
    dst.size = {32, 80, 32};
    dst.resources = {1, 1};
    ImageInfo src = dst;
    src.type = ImageType::e2D;
    src.size.depth = 1;
    for (s32 slice = 0; slice < 32; ++slice) {
        const auto copies = MakeShrinkImageCopies(dst, src, {0, slice}, 1, 0);
        Check(copies.size() == 1, "one copy per 2D source slice");
        const auto& c = copies.at(0);
        Check(c.src_subresource.base_layer == 0 && c.src_subresource.num_layers == 1 &&
              c.src_offset.z == 0, "2D source uses one layer and z zero");
        Check(c.dst_subresource.base_layer == 0 && c.dst_subresource.num_layers == 1 &&
              c.dst_offset.z == slice, "3D destination uses z offset rather than array layer");
        Check(c.extent.width == 32 && c.extent.height == 80 && c.extent.depth == 1,
              "copy is exactly one slice with matching width and height");
        Check(c.dst_offset.z + c.extent.depth <= dst.size.depth,
              "all captured slice indices stay within the volume");
    }
    src.type = ImageType::e3D;
    src.size.depth = 8;
    const auto volume = MakeShrinkImageCopies(dst, src, {0, 4}, 1, 0);
    Check(volume.at(0).extent.depth == 8 && volume.at(0).dst_offset.z == 4,
          "existing volume-to-volume copy retains its depth and offset");
    dst.type = ImageType::e2D; dst.size = {32, 80, 1};
    src = dst;
    const auto planar = MakeShrinkImageCopies(dst, src, {0, 0}, 2, 0);
    Check(planar.at(0).extent.width == 64 && planar.at(0).extent.height == 160 &&
          planar.at(0).extent.depth == 1, "existing 2D scaling is unchanged");
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--copies") Copies(); else Views();
    return failures ? 1 : 0;
}
