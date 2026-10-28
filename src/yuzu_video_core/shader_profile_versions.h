// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_layout.h"
#include "yuzu_video_core/shader_profile_location.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace VideoCommon::ShaderProfile {
struct RecordingVersion {
    std::string context, label, short_label, filename;
    std::filesystem::path directory, recording;
    std::filesystem::file_time_type recorded{};
};
struct RecordingVersions {
    std::vector<RecordingVersion> entries;
    size_t unavailable{};
};
inline std::string ProfileTitleId(u64 title) {
    auto id=ProfileFilename(title);id.resize(id.size()-9);
    return id;
}
inline std::string ReadableProfileFilename(std::string_view game,const JsonValue& provenance,u64 title) {
    return FilenamePart(std::string(game))+" - "+FilenamePart(ShortVersionLabel(provenance))+" ["+ProfileTitleId(title)+"].nxshader";
}
// "Game - Update 3.0.3 + Update 4.0.1 [id].nxshader"; ranges for longer lists.
inline std::string CombinedProfileFilename(std::string_view game,const std::vector<const RecordingVersion*>& versions,u64 title) {
    if(versions.size()==1)return versions.front()->filename;
    std::string list;
    if(versions.size()<=3) {
        for(const auto* version:versions)list+=(list.empty()?"":" + ")+version->short_label;
    } else list=versions.front()->short_label+" to "+versions.back()->short_label;
    return FilenamePart(std::string(game))+" - "+FilenamePart(list)+" ["+ProfileTitleId(title)+"].nxshader";
}
// Orders "1.10.0" after "1.9.0".
inline bool VersionLess(std::string_view a,std::string_view b) {
    const auto number=[](std::string_view& text) {
        u64 value{};size_t digits{};
        while(!text.empty() && text.front()>='0' && text.front()<='9' && digits<18) {
            value=value*10+static_cast<u64>(text.front()-'0');text.remove_prefix(1);++digits;
        }
        return digits?std::optional<u64>(value):std::nullopt;
    };
    while(!a.empty() && !b.empty()) {
        const auto x=number(a),y=number(b);
        if(x && y) { if(*x!=*y)return *x<*y; continue; }
        if(x || y)return bool(x);
        if(a.front()!=b.front())return a.front()<b.front();
        a.remove_prefix(1);b.remove_prefix(1);
    }
    return a.size()<b.size();
}
// Pipelines recorded for one version, for an informational overlap only: the
// frames are not verified here and the result never selects any shader.
inline std::set<std::string> VersionPipelineIds(const RecordingVersion& version) {
    std::set<std::string> ids;
    std::ifstream in(version.directory/"vulkan.idx",std::ios::binary);
    std::string header(72,'\0'),frame(64,'\0');
    if(!in.read(header.data(),header.size()) || header!="NXCTX002"+version.context)return ids;
    while(ids.size()<MaxVersionPipelines && in.read(frame.data(),frame.size()))ids.insert(frame.substr(0,32));
    return ids;
}
// Percentage of the smaller version's pipelines that the other version also uses.
inline unsigned SharedPipelinePercent(const RecordingVersion& a,const RecordingVersion& b) {
    const auto x=VersionPipelineIds(a),y=VersionPipelineIds(b);
    const auto& fewer=x.size()<y.size()?x:y;const auto& more=x.size()<y.size()?y:x;
    if(fewer.empty())return 0;
    size_t shared{};
    for(const auto& id:fewer)shared+=more.contains(id);
    return static_cast<unsigned>(shared*100/fewer.size());
}
// Enumerate only identified recordings, oldest game version first.
// Missing/tampered provenance is reported, never guessed from a folder name.
inline RecordingVersions ListRecordedVersions(const std::filesystem::path& cache,u64 title,
                                              std::string_view game) {
    RecordingVersions result;
    std::error_code error;
    for(auto it=std::filesystem::directory_iterator(VersionsDirectory(cache),error);
        !error && it!=std::filesystem::directory_iterator{};it.increment(error)) {
        const auto recording=it->path()/"recipes.nxr";
        std::error_code missing;
        if(!it->is_directory(missing) || !std::filesystem::is_regular_file(recording,missing))continue;
        if(result.entries.size()>=64){++result.unavailable;continue;}
        try {
            const auto context=VersionContext(it->path());
            const auto provenance=RecordedProvenance(cache,context,title);
            if(provenance.isNull()){++result.unavailable;continue;}
            result.entries.push_back({context,RecordedVersionLabel(provenance),ShortVersionLabel(provenance),
                ReadableProfileFilename(game,provenance,title),it->path(),recording,
                std::filesystem::last_write_time(recording)});
        } catch(const std::exception&) {++result.unavailable;}
    }
    // Base-only recordings first, then updates; numeric version order inside each.
    std::sort(result.entries.begin(),result.entries.end(),[](const auto& a,const auto& b){
        const bool au=a.short_label.starts_with("Update"),bu=b.short_label.starts_with("Update");
        if(au!=bu)return !au;
        if(VersionLess(a.short_label,b.short_label))return true;
        if(VersionLess(b.short_label,a.short_label))return false;
        return a.context<b.context;
    });
    std::map<std::string,size_t> names;
    for(const auto& entry:result.entries)++names[entry.filename];
    for(auto& entry:result.entries)if(names[entry.filename]>1) {
        entry.filename.insert(entry.filename.size()-9," - "+entry.context.substr(0,8));
        entry.short_label+=" ("+entry.context.substr(0,8)+")";
    }
    return result;
}
} // namespace VideoCommon::ShaderProfile
