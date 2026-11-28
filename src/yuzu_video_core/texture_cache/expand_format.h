// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/texture_cache/types.h"

namespace VideoCommon {

[[nodiscard]] bool IsPixelFormatExpanded(VideoCore::Surface::PixelFormat format);
[[nodiscard]] u32 ExpandedBytesPerTexel(VideoCore::Surface::PixelFormat format);

void ExpandPixelFormat(std::span<const u8> input, std::span<u8> output,
                       const BufferImageCopy& copy, VideoCore::Surface::PixelFormat format);

} // namespace VideoCommon
