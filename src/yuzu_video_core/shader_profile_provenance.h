// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_text.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace VideoCommon::ShaderProfile {
inline JsonValue ContentProvenance(u64 title, const std::string & base, const std::string & update,
                                  const std::string & base_version, const std::string & update_version,
                                  bool available, bool enabled) {
    if (base.empty() || enabled != !update.empty() || (enabled && !available) ||
        base_version.size() > 128 || update_version.size() > 128)
        throw std::runtime_error("Inconsistent captured content provenance");
    JsonValue value;
    value["schema"] = 1U;
    value["title"] = std::to_string(title);
    value["context"] = ContentContext(title, base, update);
    value["base_program"] = base;
    value["update_program"] = update;
    value["base_version"] = base_version;
    value["update_version"] = enabled ? update_version : "";
    value["update_available"] = available;
    value["update_enabled"] = enabled;
    return value;
}
// "Update 4.0.1" or "Base 1.0.0": the version a file or folder is named after.
inline std::string ShortVersionLabel(const JsonValue & provenance) {
    return provenance["update_enabled"].asBool() ? "Update " + provenance["update_version"].asString()
                                                 : "Base " + provenance["base_version"].asString();
}
// A small flat JSON object of a version folder; null when the file is absent.
inline JsonValue ReadVersionFile(const std::filesystem::path & path) {
    if (!std::filesystem::exists(path)) return {};
    if (std::filesystem::file_size(path) > 16384) throw std::runtime_error("Capture provenance size limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read captured version");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    // This metadata is a flat object. Bound nesting before the generic reader;
    // the UI need not link the shader exporter just to display a version.
    bool quoted = false, escaped = false;
    unsigned depth{};
    for (char c : bytes) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '[' || (c == '{' && ++depth > 1)) throw std::runtime_error("Invalid capture provenance structure");
        else if (c == '}') { if (!depth) throw std::runtime_error("Invalid capture provenance structure"); --depth; }
    }
    JsonValue value;
    JsonReader reader;
    if (!reader.Parse(bytes.data(), bytes.data() + bytes.size(), value) || !value.isObject())
        throw std::runtime_error("Invalid captured version metadata");
    return value;
}
inline JsonValue RecordedProvenance(const std::filesystem::path & cache, std::string_view context, u64 title) {
    if (!ValidContext(context)) return {};
    const auto directory = FindVersionDirectory(cache, context);
    if (directory.empty()) return {};
    auto value = ReadVersionFile(directory / "version.json");
    if (value.isNull()) return {};
    if (value["schema"].asUInt64() != 1 || value["title"].asString() != std::to_string(title) ||
        value["context"].asString() != context || !value["update_available"].isBool() || !value["update_enabled"].isBool())
        throw std::runtime_error("Captured version does not match the recording");
    const auto expected = ContentProvenance(title, value["base_program"].asString(), value["update_program"].asString(),
        value["base_version"].asString(), value["update_version"].asString(),
        value["update_available"].asBool(), value["update_enabled"].asBool());
    if (expected["context"].asString() != context) throw std::runtime_error("Captured program identity mismatch");
    return expected;
}
// A version known only from an imported file, which gives just its version numbers.
// It has no programs to verify: the numbers name it and go into its exports.
inline JsonValue ImportedVersion(u64 title, std::string_view context, const std::string & base, const std::string & update) {
    if (!ValidContext(context) || (base.empty() && update.empty()) ||
        !ValidProfileText(base, MaxVersionText) || !ValidProfileText(update, MaxVersionText))
        return {};
    JsonValue value;
    value["schema"] = 1U;
    value["title"] = std::to_string(title);
    value["context"] = std::string(context);
    value["base_version"] = base;
    value["update_version"] = update;
    value["update_available"] = !update.empty();
    value["update_enabled"] = !update.empty();
    return value;
}
inline std::filesystem::path ImportedVersionPath(const std::filesystem::path & directory) {
    return directory / "imported-version.json";
}
// What is known of a version: its recorded provenance, else the numbers of an import.
inline JsonValue VersionProvenance(const std::filesystem::path & cache, std::string_view context, u64 title) {
    if (!ValidContext(context)) return {};
    if (auto recorded = RecordedProvenance(cache, context, title); !recorded.isNull()) return recorded;
    const auto directory = FindVersionDirectory(cache, context);
    if (directory.empty()) return {};
    const auto value = ReadVersionFile(ImportedVersionPath(directory));
    if (value.isNull()) return {};
    auto expected = ImportedVersion(title, context, value["base_version"].asString(), value["update_version"].asString());
    if (expected.isNull() || value["schema"].asUInt64() != 1 || value["title"].asString() != std::to_string(title) ||
        value["context"].asString() != context)
        throw std::runtime_error("Imported version does not match its folder");
    return expected;
}
inline void PublishVersionFile(const std::filesystem::path & path, const JsonValue & value) {
    const auto bytes = Canonical(value);
    auto temporary = path; temporary += ".partial";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size()); out.flush(); out.close();
    if (!out) throw std::runtime_error("Cannot save captured version");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish captured version");
#else
    std::filesystem::rename(temporary, path);
#endif
}
inline void SaveProvenance(const std::filesystem::path & cache, const JsonValue & value) {
    const auto context = value["context"].asString();
    if (!ValidContext(context)) throw std::runtime_error("Invalid provenance context");
    PublishVersionFile(VersionDirectory(cache, context, ShortVersionLabel(value)) / "version.json", value);
}
// Kept only while the version has no recorded provenance, which describes it better.
inline void SaveImportedVersion(const std::filesystem::path & cache, const JsonValue & value) {
    const auto context = value["context"].asString();
    if (!ValidContext(context)) throw std::runtime_error("Invalid provenance context");
    if (const auto directory = FindVersionDirectory(cache, context);
        !directory.empty() && std::filesystem::exists(directory / "version.json"))
        return;
    PublishVersionFile(ImportedVersionPath(VersionDirectory(cache, context, ShortVersionLabel(value))), value);
}
inline std::string RecordedVersionLabel(const JsonValue & provenance) {
    if (provenance.isNull()) return "Recorded version metadata unavailable";
    const auto base = provenance["base_version"].asString();
    std::string label = "Base " + (base.empty() ? std::string("version unavailable") : base);
    if (provenance["update_enabled"].asBool()) {
        const auto version = provenance["update_version"].asString();
        label += "; update " + (version.empty() ? std::string("version unavailable") : version) + " enabled";
    } else label += provenance["update_available"].asBool() ? "; update disabled" : "; no update";
    return label;
}
} // namespace VideoCommon::ShaderProfile
