// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_export.h"
#ifdef _WIN32
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
inline JsonValue RecordedProvenance(const std::filesystem::path & cache, std::string_view context, u64 title) {
    if (!ValidContext(context)) return {};
    const auto directory = FindVersionDirectory(cache, context);
    if (directory.empty()) return {};
    const auto path = directory / "version.json";
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
    if (!reader.Parse(bytes.data(), bytes.data() + bytes.size(), value)) throw std::runtime_error("Invalid captured version metadata");
    if (!value.isObject() || value["schema"].asUInt64() != 1 || value["title"].asString() != std::to_string(title) ||
        value["context"].asString() != context || !value["update_available"].isBool() || !value["update_enabled"].isBool())
        throw std::runtime_error("Captured version does not match the recording");
    const auto expected = ContentProvenance(title, value["base_program"].asString(), value["update_program"].asString(),
        value["base_version"].asString(), value["update_version"].asString(),
        value["update_available"].asBool(), value["update_enabled"].asBool());
    if (expected["context"].asString() != context) throw std::runtime_error("Captured program identity mismatch");
    return expected;
}
inline void SaveProvenance(const std::filesystem::path & cache, const JsonValue & value) {
    const auto context = value["context"].asString();
    if (!ValidContext(context)) throw std::runtime_error("Invalid provenance context");
    const auto path = VersionDirectory(cache, context, ShortVersionLabel(value)) / "version.json";
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
