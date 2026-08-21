#pragma once
#include <vector>

enum VulkanCheckResult {
    EXIT_VULKAN_AVAILABLE = 0,
    EXIT_VULKAN_NOT_AVAILABLE = 1,
    VULKAN_CHECK_DONE = 2
};

VulkanCheckResult RunVulkanProbeIfChild();
VulkanCheckResult StartupVulkanChecks();

class VkDeviceRecord {
public:
    explicit VkDeviceRecord(const std::string & name, const std::vector<uint32_t> & vsync_modes, bool has_broken_compute);
    ~VkDeviceRecord();

    const std::string name;
    const std::vector<uint32_t> vsync_support;
    const bool has_broken_compute;
};
void PopulateVulkanRecords(std::vector<VkDeviceRecord> & records, void * renderSurface);