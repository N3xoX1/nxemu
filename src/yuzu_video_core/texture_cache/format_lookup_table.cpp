// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>

#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/texture_cache/format_lookup_table.h"

namespace VideoCommon {

using Tegra::Texture::ComponentType;
using Tegra::Texture::TextureFormat;
using VideoCore::Surface::PixelFormat;

namespace {

constexpr auto SNORM = ComponentType::SNORM;
constexpr auto UNORM = ComponentType::UNORM;
constexpr auto SINT = ComponentType::SINT;
constexpr auto UINT = ComponentType::UINT;
constexpr auto FLOAT = ComponentType::FLOAT;
constexpr bool LINEAR = false;
constexpr bool SRGB = true;

constexpr u32 Hash(TextureFormat format, ComponentType red_component, ComponentType green_component,
                   ComponentType blue_component, ComponentType alpha_component, bool is_srgb) {
    u32 hash = is_srgb ? 1 : 0;
    hash |= static_cast<u32>(red_component) << 1;
    hash |= static_cast<u32>(green_component) << 4;
    hash |= static_cast<u32>(blue_component) << 7;
    hash |= static_cast<u32>(alpha_component) << 10;
    hash |= static_cast<u32>(format) << 13;
    return hash;
}

constexpr u32 Hash(TextureFormat format, ComponentType component, bool is_srgb = LINEAR) {
    return Hash(format, component, component, component, component, is_srgb);
}

struct Entry {
    TextureFormat format;
    ComponentType red, green, blue, alpha;
    bool srgb;
    PixelFormat pixel_format;
};
// Every texture descriptor format the renderer supports and its pixel format.
constexpr std::array<Entry, 101> Table{{
    {TextureFormat::A8B8G8R8, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::A8B8G8R8_UNORM},
    {TextureFormat::A8B8G8R8, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::A8B8G8R8_SNORM},
    {TextureFormat::A8B8G8R8, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::A8B8G8R8_UINT},
    {TextureFormat::A8B8G8R8, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::A8B8G8R8_SINT},
    {TextureFormat::A8B8G8R8, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::A8B8G8R8_SRGB},
    {TextureFormat::B5G6R5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::B5G6R5_UNORM},
    {TextureFormat::A2B10G10R10, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::A2B10G10R10_UNORM},
    {TextureFormat::A2B10G10R10, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::A2B10G10R10_UINT},
    {TextureFormat::A1B5G5R5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::A1B5G5R5_UNORM},
    {TextureFormat::A4B4G4R4, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::A4B4G4R4_UNORM},
    {TextureFormat::G4R4, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::G4R4_UNORM},
    {TextureFormat::A5B5G5R1, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::A5B5G5R1_UNORM},
    {TextureFormat::R8, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::R8_UNORM},
    {TextureFormat::R8, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::R8_SNORM},
    {TextureFormat::R8, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R8_UINT},
    {TextureFormat::R8, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R8_SINT},
    {TextureFormat::G8R8, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::R8G8_UNORM},
    {TextureFormat::G8R8, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::R8G8_SNORM},
    {TextureFormat::G8R8, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R8G8_UINT},
    {TextureFormat::G8R8, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R8G8_SINT},
    {TextureFormat::R16G16B16A16, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R16G16B16A16_FLOAT},
    {TextureFormat::R16G16B16A16, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::R16G16B16A16_UNORM},
    {TextureFormat::R16G16B16A16, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::R16G16B16A16_SNORM},
    {TextureFormat::R16G16B16A16, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R16G16B16A16_UINT},
    {TextureFormat::R16G16B16A16, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R16G16B16A16_SINT},
    {TextureFormat::R16G16, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R16G16_FLOAT},
    {TextureFormat::R16G16, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::R16G16_UNORM},
    {TextureFormat::R16G16, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::R16G16_SNORM},
    {TextureFormat::R16G16, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R16G16_UINT},
    {TextureFormat::R16G16, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R16G16_SINT},
    {TextureFormat::R16, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R16_FLOAT},
    {TextureFormat::R16, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::R16_UNORM},
    {TextureFormat::R16, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::R16_SNORM},
    {TextureFormat::R16, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R16_UINT},
    {TextureFormat::R16, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R16_SINT},
    {TextureFormat::B10G11R11, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::B10G11R11_FLOAT},
    {TextureFormat::R32G32B32A32, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R32G32B32A32_FLOAT},
    {TextureFormat::R32G32B32A32, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R32G32B32A32_UINT},
    {TextureFormat::R32G32B32A32, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R32G32B32A32_SINT},
    {TextureFormat::R32G32B32, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R32G32B32_FLOAT},
    {TextureFormat::R32G32, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R32G32_FLOAT},
    {TextureFormat::R32G32, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R32G32_UINT},
    {TextureFormat::R32G32, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R32G32_SINT},
    {TextureFormat::R32, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::R32_FLOAT},
    {TextureFormat::R32, UINT, UINT, UINT, UINT, LINEAR, PixelFormat::R32_UINT},
    {TextureFormat::R32, SINT, SINT, SINT, SINT, LINEAR, PixelFormat::R32_SINT},
    {TextureFormat::E5B9G9R9, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::E5B9G9R9_FLOAT},
    {TextureFormat::Z32, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::D32_FLOAT},
    {TextureFormat::Z32, FLOAT, UINT, UINT, UINT, LINEAR, PixelFormat::D32_FLOAT},
    {TextureFormat::Z16, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::D16_UNORM},
    {TextureFormat::Z16, UNORM, UINT, UINT, UINT, LINEAR, PixelFormat::D16_UNORM},
    {TextureFormat::X8Z24, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::X8_D24_UNORM},
    {TextureFormat::X8Z24, UNORM, UINT, UINT, UINT, LINEAR, PixelFormat::X8_D24_UNORM},
    {TextureFormat::Z24S8, UINT, UNORM, UNORM, UNORM, LINEAR, PixelFormat::S8_UINT_D24_UNORM},
    {TextureFormat::Z24S8, UINT, UNORM, UINT, UINT, LINEAR, PixelFormat::S8_UINT_D24_UNORM},
    {TextureFormat::G24R8, UINT, UNORM, UNORM, UNORM, LINEAR, PixelFormat::S8_UINT_D24_UNORM},
    {TextureFormat::S8Z24, UNORM, UINT, UINT, UINT, LINEAR, PixelFormat::D24_UNORM_S8_UINT},
    {TextureFormat::Z32_X24S8, FLOAT, UINT, UNORM, UNORM, LINEAR, PixelFormat::D32_FLOAT_S8_UINT},
    {TextureFormat::R32B24G8, FLOAT, UINT, UNORM, UNORM, LINEAR, PixelFormat::D32_FLOAT_S8_UINT},
    {TextureFormat::DXT1, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC1_RGBA_UNORM},
    {TextureFormat::DXT1, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::BC1_RGBA_SRGB},
    {TextureFormat::DXT23, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC2_UNORM},
    {TextureFormat::DXT23, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::BC2_SRGB},
    {TextureFormat::DXT45, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC3_UNORM},
    {TextureFormat::DXT45, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::BC3_SRGB},
    {TextureFormat::DXN1, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC4_UNORM},
    {TextureFormat::DXN1, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::BC4_SNORM},
    {TextureFormat::DXN2, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC5_UNORM},
    {TextureFormat::DXN2, SNORM, SNORM, SNORM, SNORM, LINEAR, PixelFormat::BC5_SNORM},
    {TextureFormat::BC7U, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::BC7_UNORM},
    {TextureFormat::BC7U, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::BC7_SRGB},
    {TextureFormat::BC6H_S16, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::BC6H_SFLOAT},
    {TextureFormat::BC6H_U16, FLOAT, FLOAT, FLOAT, FLOAT, LINEAR, PixelFormat::BC6H_UFLOAT},
    {TextureFormat::ASTC_2D_4X4, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_4X4_UNORM},
    {TextureFormat::ASTC_2D_4X4, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_4X4_SRGB},
    {TextureFormat::ASTC_2D_5X4, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_5X4_UNORM},
    {TextureFormat::ASTC_2D_5X4, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_5X4_SRGB},
    {TextureFormat::ASTC_2D_5X5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_5X5_UNORM},
    {TextureFormat::ASTC_2D_5X5, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_5X5_SRGB},
    {TextureFormat::ASTC_2D_8X8, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_8X8_UNORM},
    {TextureFormat::ASTC_2D_8X8, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_8X8_SRGB},
    {TextureFormat::ASTC_2D_8X5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_8X5_UNORM},
    {TextureFormat::ASTC_2D_8X5, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_8X5_SRGB},
    {TextureFormat::ASTC_2D_10X8, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_10X8_UNORM},
    {TextureFormat::ASTC_2D_10X8, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_10X8_SRGB},
    {TextureFormat::ASTC_2D_6X6, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_6X6_UNORM},
    {TextureFormat::ASTC_2D_6X6, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_6X6_SRGB},
    {TextureFormat::ASTC_2D_10X6, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_10X6_UNORM},
    {TextureFormat::ASTC_2D_10X6, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_10X6_SRGB},
    {TextureFormat::ASTC_2D_10X5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_10X5_UNORM},
    {TextureFormat::ASTC_2D_10X5, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_10X5_SRGB},
    {TextureFormat::ASTC_2D_10X10, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_10X10_UNORM},
    {TextureFormat::ASTC_2D_10X10, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_10X10_SRGB},
    {TextureFormat::ASTC_2D_12X10, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_12X10_UNORM},
    {TextureFormat::ASTC_2D_12X10, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_12X10_SRGB},
    {TextureFormat::ASTC_2D_12X12, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_12X12_UNORM},
    {TextureFormat::ASTC_2D_12X12, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_12X12_SRGB},
    {TextureFormat::ASTC_2D_8X6, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_8X6_UNORM},
    {TextureFormat::ASTC_2D_8X6, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_8X6_SRGB},
    {TextureFormat::ASTC_2D_6X5, UNORM, UNORM, UNORM, UNORM, LINEAR, PixelFormat::ASTC_2D_6X5_UNORM},
    {TextureFormat::ASTC_2D_6X5, UNORM, UNORM, UNORM, UNORM, SRGB, PixelFormat::ASTC_2D_6X5_SRGB},
}};

constexpr u32 Hash(const Entry& entry) {
    return Hash(entry.format, entry.red, entry.green, entry.blue, entry.alpha, entry.srgb);
}

constexpr auto SortedTable = [] {
    auto sorted = Table;
    std::ranges::sort(sorted, {}, [](const Entry& entry) { return Hash(entry); });
    return sorted;
}();
static_assert(std::ranges::adjacent_find(SortedTable, {}, [](const Entry& entry) { return Hash(entry); }) ==
              SortedTable.end());

} // Anonymous namespace

std::optional<PixelFormat> FindPixelFormat(TextureFormat format, ComponentType red, ComponentType green,
                                           ComponentType blue, ComponentType alpha, bool is_srgb) noexcept {
    const u32 hash = Hash(format, red, green, blue, alpha, is_srgb);
    const auto it = std::ranges::lower_bound(SortedTable, hash, {}, [](const Entry& entry) { return Hash(entry); });
    if (it != SortedTable.end() && Hash(*it) == hash) {
        return it->pixel_format;
    }
    return std::nullopt;
}

PixelFormat PixelFormatFromTextureInfo(TextureFormat format, ComponentType red, ComponentType green,
                                       ComponentType blue, ComponentType alpha,
                                       bool is_srgb) noexcept {
    if (const auto pixel_format = FindPixelFormat(format, red, green, blue, alpha, is_srgb)) {
        return *pixel_format;
    }
    UNIMPLEMENTED_MSG("texture format={} srgb={} components={{{} {} {} {}}}",
                      static_cast<int>(format), is_srgb, static_cast<int>(red),
                      static_cast<int>(green), static_cast<int>(blue), static_cast<int>(alpha));
    return PixelFormat::A8B8G8R8_UNORM;
}

std::optional<TextureDescriptorFormat> TextureDescriptorFromPixelFormat(PixelFormat pixel_format) noexcept {
    // Table order: the first descriptor of a pixel format is its canonical one.
    for (const auto& entry : Table) {
        if (entry.pixel_format == pixel_format) {
            return TextureDescriptorFormat{entry.format, entry.red, entry.green, entry.blue, entry.alpha,
                                           entry.srgb};
        }
    }
    return std::nullopt;
}

} // namespace VideoCommon
