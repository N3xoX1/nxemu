// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include "yuzu_common/common_types.h"

namespace Common::Compression {

[[nodiscard]] bool IsZBIC(std::span<const u8> src);

// Succeeds only if the frame produces exactly the caller's expected segment size.
[[nodiscard]] bool DecompressDataZBIC(std::span<u8> dst, std::span<const u8> src);

} // namespace Common::Compression
