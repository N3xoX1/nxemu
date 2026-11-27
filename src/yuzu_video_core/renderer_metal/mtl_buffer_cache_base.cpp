// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_video_core/buffer_cache/buffer_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_buffer_cache.h"

namespace VideoCommon {
template class VideoCommon::BufferCache<Metal::BufferCacheParams>;
}
