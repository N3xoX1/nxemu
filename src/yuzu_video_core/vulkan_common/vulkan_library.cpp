// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <string>

#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"

namespace Vulkan
{

#ifdef __APPLE__
namespace {

std::filesystem::path AppleVulkanLibraryPath()
{
    const auto exe_dir = Common::FS::GetExeDirectory();
    if (exe_dir.filename() == "MacOS" && exe_dir.parent_path().filename() == "Contents") {
        return exe_dir.parent_path() / "Frameworks/libMoltenVK.dylib";
    }
    return exe_dir.empty() ? std::filesystem::path{} : exe_dir / "libMoltenVK.dylib";
}

} // namespace
#endif

std::shared_ptr<Common::DynamicLibrary> OpenLibrary([[maybe_unused]] Core::Frontend::GraphicsContext * context)
{
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(ANDROID)
    if (context != nullptr)
    {
        std::shared_ptr<Common::DynamicLibrary> context_library = context->GetDriverLibrary();
        if (context_library && context_library->IsOpen())
        {
            return context_library;
        }
    }
    std::shared_ptr<Common::DynamicLibrary> library = std::make_shared<Common::DynamicLibrary>();
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: libvulkan.so");
    void(library->Open("libvulkan.so"));
    return library;
#else
    std::shared_ptr<Common::DynamicLibrary> library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    const auto library_path = AppleVulkanLibraryPath();
    if (!library_path.empty())
    {
        const std::string path = library_path.string();
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", path);
        if (library->Open(path.c_str()))
        {
            LOG_INFO(Render_Vulkan, "Loaded Vulkan library: {}", path);
        }
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str()))
    {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan
