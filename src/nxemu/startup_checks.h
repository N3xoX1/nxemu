// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(NXEMU_UNIX)
#include <sys/types.h>
#endif

constexpr char IS_CHILD_ENV_VAR[] = "NXEMU_IS_CHILD";
constexpr char STARTUP_CHECK_ENV_VAR[] = "NXEMU_DO_STARTUP_CHECKS";
constexpr char ENV_VAR_ENABLED_TEXT[] = "ON";

void CheckVulkan();
bool CheckEnvVars(bool* is_child);
bool StartupChecks(const char* arg0, bool* has_broken_vulkan, bool perform_vulkan_check);

class VkDeviceRecord {
public:
    explicit VkDeviceRecord(const std::string & name, const std::vector<uint32_t> & vsync_modes, bool has_broken_compute);
    ~VkDeviceRecord();

    const std::string name;
    const std::vector<uint32_t> vsync_support;
    const bool has_broken_compute;
};
void PopulateVulkanRecords(std::vector<VkDeviceRecord> & records, void * renderSurface);

#ifdef _WIN32
bool SpawnChild(const char* arg0, PROCESS_INFORMATION* pi, int flags);
#elif defined(NXEMU_UNIX)
pid_t SpawnChild(const char* arg0);
#endif
