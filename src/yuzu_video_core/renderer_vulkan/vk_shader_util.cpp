// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "yuzu_common/common_types.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    try {
        return device.GetLogical().CreateShaderModule({
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = static_cast<u32>(code.size_bytes()),
            .pCode = code.data(),
        });
    } catch (const vk::Exception& exception) {
        // Hash only on failure. This identifies the offending built-in SPIR-V
        // even when its caller does not carry a shader name.
        u64 hash = 14695981039346656037ULL;
        for (u32 word : code) {
            hash ^= word;
            hash *= 1099511628211ULL;
        }
        LOG_ERROR(Render_Vulkan, "[VK-STARTUP] Built-in SPIR-V module failed: {}, words={}, "
                  "magic=0x{:08x}, fingerprint=0x{:016x}", exception.what(), code.size(),
                  code.empty() ? 0U : code.front(), hash);
        throw;
    }
}

} // namespace Vulkan
