// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/textures/texture.h"

namespace VideoCommon {

VideoCore::Surface::PixelFormat PixelFormatFromTextureInfo(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

/// The same lookup without logging: nullopt for a combination the renderer does not support.
std::optional<VideoCore::Surface::PixelFormat> FindPixelFormat(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

struct TextureDescriptorFormat {
    Tegra::Texture::TextureFormat format;
    Tegra::Texture::ComponentType red, green, blue, alpha;
    bool srgb;
};

/// The first texture descriptor format that gives this pixel format, if one does.
std::optional<TextureDescriptorFormat> TextureDescriptorFromPixelFormat(
    VideoCore::Surface::PixelFormat pixel_format) noexcept;

} // namespace VideoCommon
