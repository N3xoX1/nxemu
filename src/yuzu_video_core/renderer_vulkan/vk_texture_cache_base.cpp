// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/texture_cache/texture_cache.h"

namespace VideoCommon {
template class VideoCommon::TextureCache<Vulkan::TextureCacheParams>;
template void VideoCommon::TextureCache<Vulkan::TextureCacheParams>::UpdateImagesFromBuffer<VkBuffer>(
    DAddr, size_t, VkBuffer, u64);
}
