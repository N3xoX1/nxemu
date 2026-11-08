// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include "yuzu_video_core/renderer_vulkan/null_image.h"
void Check(bool ok) { if (!ok) throw std::runtime_error("null image description regression"); }
int main() {
    using namespace Vulkan::NullImage;
    for (size_t i = 0; i < Count; ++i) {
        const auto image = ImageInfo(i);
        Check(image.imageType == Types[i] && image.extent.width == 1 &&
              image.extent.height == 1 && image.extent.depth == 1);
        Check(image.arrayLayers == (i == 1 ? 6U : 1U));
        Check((image.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0);
        Check((image.usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
        Check(((image.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0) == (i == 1));
    }
    for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; ++i) {
        const auto type = static_cast<Shader::TextureType>(i);
        if (type == Shader::TextureType::Buffer) continue;
        const auto image = ImageInfo(Index(type));
        const auto view = ViewInfo(type, VK_NULL_HANDLE);
        Check(view.format == image.format && view.subresourceRange.layerCount <= image.arrayLayers);
        Check(view.subresourceRange.levelCount == 1 && view.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
        switch (view.viewType) {
        case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
            Check(image.imageType == VK_IMAGE_TYPE_1D); break;
        case VK_IMAGE_VIEW_TYPE_3D:
            Check(image.imageType == VK_IMAGE_TYPE_3D && image.arrayLayers == 1); break;
        case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            Check(image.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
            Check(view.subresourceRange.layerCount == 6);
            [[fallthrough]];
        default: Check(image.imageType == VK_IMAGE_TYPE_2D); break;
        }
    }
}
