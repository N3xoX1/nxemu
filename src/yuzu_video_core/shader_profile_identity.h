// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nxemu-module-spec/system_loader.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_provenance.h"
#include <filesystem>
#include <memory>
#include <algorithm>

namespace VideoCommon::ShaderProfile
{
template <class T>
struct ReleaseInterface
{
    void operator()(T * p) const
    {
        if (p) p->Release();
    }
};
template <class T>
using InterfacePtr = std::unique_ptr<T, ReleaseInterface<T>>;

struct LoadedContentDescription {
    std::string context;
    JsonValue provenance;
};
// Startup only. The loader supplies the selected version, including an older
// installed update. Enabled layered mods remain deliberately unidentified.
inline LoadedContentDescription DescribeLoadedContent(ISystemloader & loader, u64 title)
{
    try
    {
        ActiveProgramContent content;
        const bool identified = loader.GetActiveProgramContent(title, content);
        InterfacePtr<IFileSysNCA> base(content.base_program), update(content.update_program);
        if (!identified) return {};
        std::string base_part, update_part;
        const auto append = [&](IFileSysNCA * nca, std::string& part) {
            if (!nca) return false;
            InterfacePtr<IVirtualFile> file(nca->GetBaseFile());
            if (!file || file->GetSize() < 0xc00) return false;
            std::string header(0xc00, '\0');
            if (file->ReadBytes(reinterpret_cast<uint8_t *>(header.data()), header.size(), 0) != header.size()) return false;
            part = ContentPart(file->GetSize(), header);
            return true;
        };
        if (!append(base.get(), base_part)) return {};
        if (content.update_enabled && !append(update.get(), update_part)) return {};
        const auto provenance = ContentProvenance(title, base_part, update_part,
            std::string(content.base_version, std::find(content.base_version, content.base_version + sizeof(content.base_version), '\0')),
            std::string(content.update_version, std::find(content.update_version, content.update_version + sizeof(content.update_version), '\0')),
            content.update_available, content.update_enabled);
        return {ContentContext(title, base_part, update_part), provenance};
    }
    catch (...)
    {
        return {};
    }
}
inline std::string LoadedContentIdentity(ISystemloader & loader, u64 title, const std::filesystem::path &) {
    return DescribeLoadedContent(loader, title).context;
}
inline std::string ImportIdentity(std::string_view profile_hash, std::string_view content,
                                  u64 title, u32 features, u32 cache_abi)
{
    if (content.empty()) return {};
    return ResourceSha256("nxshader-import-2:" + std::string(profile_hash) + ":" + std::string(content) + ":" +
                          std::to_string(title) + ":" + std::to_string(features) + ":" + std::to_string(cache_abi));
}
} // namespace VideoCommon::ShaderProfile
