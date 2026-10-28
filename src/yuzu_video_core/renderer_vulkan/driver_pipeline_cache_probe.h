// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstring>
#include <span>
#include <string_view>
#include <vulkan/vulkan_core.h>

namespace Vulkan::DriverCacheProbe {
inline bool EnabledFromOption(const char* option) {
    return option && std::string_view(option) == "1";
}

// Diagnostic classification only. Never decides whether to load or delete a
// cache, and never claims a native MSL/library/PSO hit.
inline const char* Header(std::span<const char> bytes,
                          const VkPhysicalDeviceProperties& properties) {
    if (bytes.size() < sizeof(VkPipelineCacheHeaderVersionOne)) return "short";
    VkPipelineCacheHeaderVersionOne header{};
    // The file may be unaligned. Copy instead of casting its storage.
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.headerSize < sizeof(header) || header.headerSize > bytes.size() ||
        header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return "invalid";
    if (header.vendorID != properties.vendorID || header.deviceID != properties.deviceID)
        return "different-device";
    if (std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return "different-uuid";
    return "compatible";
}
} // namespace Vulkan::DriverCacheProbe
