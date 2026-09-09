// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"

namespace Vulkan
{

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
    const std::filesystem::path libvulkan_filename = Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const std::filesystem::path libmoltenvk_filename = Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    const char * library_paths[] = {std::getenv("LIBVULKAN_PATH"), libvulkan_filename.c_str(), libmoltenvk_filename.c_str()};
    // Check if a path to a specific Vulkan library has been specified.
    for (const char * library_path : library_paths)
    {
        if (library_path && library->Open(library_path))
        {
            break;
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
