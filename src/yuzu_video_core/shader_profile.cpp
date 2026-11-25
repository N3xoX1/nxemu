// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_metrics.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "yuzu_video_core/shader_profile_location.h"
#include "common/json.h"
#include "common/sha256.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <cstring>

namespace VideoCommon::ShaderProfile {
using Timing::Phase;
using Timing::Counter;
using Timing::Measure;
namespace {
constexpr size_t MaxResource = 512*1024*1024;
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::string Bytes(const std::filesystem::path& path, u64 offset, u64 size, size_t limit=MaxResource,
                  Timing::Metrics* timings=nullptr) {
    Timing::Timer timer(timings,Phase::ResourceIO);
    Check(size <= limit,"Profile resource too large");
    const auto total=std::filesystem::file_size(path);
    Check(offset <= total && size <= total-offset,"Profile resource bounds");
    std::ifstream file(path,std::ios::binary); file.seekg(static_cast<std::streamoff>(offset));
    std::string out(static_cast<size_t>(size),'\0');
    file.read(out.data(),static_cast<std::streamsize>(size)); Check(bool(file),"Cannot read profile resource");
    if(timings) timings->Add(Counter::ResourceReadBytes,size);
    return out;
}
} // namespace

std::filesystem::path FindProfileForContext(const std::vector<std::filesystem::path>& directories,
                                          u64 title_id,std::string_view context) {
    const auto legacy=ProfileFilename(title_id);
    const auto id=legacy.substr(0,16);
    std::filesystem::path fallback;
    std::filesystem::path verified_version;
    for(const auto& directory:directories) {
        if(directory.empty())continue;
        std::error_code error;
        if(!std::filesystem::is_directory(directory,error))continue;
        std::vector<std::filesystem::path> candidates;
        const auto canonical=directory/legacy;
        if(std::filesystem::is_regular_file(canonical,error))candidates.push_back(canonical);
        for(auto it=std::filesystem::directory_iterator(directory,error);
            !error && it!=std::filesystem::directory_iterator{};it.increment(error)) {
            if(candidates.size()>=64)break;
            const auto name=it->path().filename().string();
            if(name==legacy || it->path().extension()!=".nxshader" || name.find(id)==std::string::npos)continue;
            if(it->is_regular_file(error))candidates.push_back(it->path());
        }
        // Stable choice when several revisions of one game were dropped together.
        std::sort(candidates.begin(),candidates.end());
        for(const auto& candidate:candidates) {
            try {
                const auto metadata=Inspect(candidate,title_id);
                if(!context.empty() && metadata.primary_context==context)return candidate;
                if(!context.empty() && std::find(metadata.contexts.begin(),metadata.contexts.end(),context)!=metadata.contexts.end()) {
                    if(verified_version.empty())verified_version=candidate;
                } else if(fallback.empty())fallback=candidate;
            } catch(const std::exception&) { /* Bad/foreign optional profiles do not block startup. */ }
        }
    }
    return verified_version.empty()?fallback:verified_version;
}

std::vector<Recipe> Read(const std::filesystem::path& path,const std::filesystem::path& root,u64 title_id,
                         size_t* unresolved_count, Timing::Metrics* timings,const ResourceProgress& progress, ImportReport* report, std::string_view target_context, const std::set<std::string>* existing_recipes, const ProgramMap* existing_programs, bool allow_cross_version) {
    Timing::Timer total(timings,Phase::ReaderTotal);
    const auto packed=Measure(timings,Phase::ProfileIO,[&] {
        return Bytes(path,0,std::filesystem::file_size(path),MaxProfileFileBytes);
    });
    Check(packed.starts_with("NXSHADR2"),"Expected a compressed NXShader profile");
    const auto payload=Measure(timings,Phase::ProfileDecompression,[&] { return Unpack(packed); });
    if(timings) timings->Add(Counter::ProfileBytes,packed.size());
    return ReadCompact(payload, root, title_id, unresolved_count, timings, progress, report, target_context, existing_recipes, existing_programs, allow_cross_version);
}
} // namespace VideoCommon::ShaderProfile
