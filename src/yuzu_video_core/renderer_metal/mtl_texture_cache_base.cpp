// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_video_core/renderer_metal/mtl_texture_cache.h"
#include "yuzu_video_core/texture_cache/texture_cache.h"

namespace VideoCommon {
template class VideoCommon::TextureCache<Metal::TextureCacheParams>;
template void VideoCommon::TextureCache<Metal::TextureCacheParams>::FillGraphicsImageViews<true>(
    std::span<ImageViewInOut>);
}
