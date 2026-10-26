// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace VideoCommon {
// Immutable environment metadata and program storage. Both are shared across
// records; serializing a view materializes at most one temporary environment.
class SharedShaderBytes {
    struct Parts { std::string metadata; std::shared_ptr<const std::string> program; };
public:
    SharedShaderBytes() : parts(std::make_shared<const Parts>()) {}
    SharedShaderBytes(std::string bytes) : parts(std::make_shared<const Parts>(Parts{std::move(bytes),{}})) {}
    // Header followed by query metadata, with the program retained separately.
    static SharedShaderBytes FromMetadata(std::string metadata, std::shared_ptr<const std::string> program) {
        if (metadata.size()<68 || !program) throw std::runtime_error("Invalid shared shader metadata");
        return Split(std::move(metadata),std::move(program));
    }
    static SharedShaderBytes FromStore(std::string_view metadata, std::shared_ptr<const std::string> program) {
        if (metadata.size()<100 || !program) throw std::runtime_error("Invalid shared shader environment");
        return Split(std::string(metadata.substr(0,68))+std::string(metadata.substr(100)),std::move(program));
    }
    static SharedShaderBytes FromEnvironment(std::string_view environment, std::shared_ptr<const std::string> program) {
        if (!program || environment.size()<68+program->size()) throw std::runtime_error("Invalid shared shader program");
        const auto offset=68+program->size();
        return Split(std::string(environment.substr(0,68))+std::string(environment.substr(offset)),std::move(program));
    }
    const void* Identity() const noexcept { return parts.get(); }
    std::shared_ptr<const std::string> ProgramStorage() const noexcept { return parts->program; }
    const void* ProgramIdentity() const noexcept { return parts->program.get(); }
    const std::string& ProgramBytes() const { return *parts->program; }
    size_t size() const noexcept { return parts->metadata.size()+(parts->program?parts->program->size():0); }
    size_t MetadataBytes() const noexcept { return parts->metadata.size(); }
    std::string String() const { return substr(0); }
    std::string substr(size_t offset, size_t count=std::string::npos) const {
        if(offset>size())throw std::out_of_range("Shared shader range");
        count=std::min(count,size()-offset);
        if(!parts->program)return parts->metadata.substr(offset,count);
        std::string result;result.reserve(count);
        const auto append=[&](std::string_view piece) {
            if(offset>=piece.size()){offset-=piece.size();return;}
            const auto n=std::min(count,piece.size()-offset);
            result.append(piece.substr(offset,n));count-=n;offset=0;
        };
        append(std::string_view(parts->metadata).substr(0,68));
        append(*parts->program);append(std::string_view(parts->metadata).substr(68));
        return result;
    }
    bool operator==(const SharedShaderBytes& other) const {
        return parts==other.parts || String()==other.String();
    }
private:
    static SharedShaderBytes Split(std::string metadata,std::shared_ptr<const std::string> program) {
        SharedShaderBytes result;
        result.parts=std::make_shared<const Parts>(Parts{std::move(metadata),std::move(program)});
        return result;
    }
    std::shared_ptr<const Parts> parts;
};
} // namespace VideoCommon
