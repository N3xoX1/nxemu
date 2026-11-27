// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

namespace Metal::Options {

namespace Detail {
inline bool Env(const char* name, bool fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::string_view{value} != "0";
}
} // namespace Detail

/// Metal API validation, for debugging only.
inline bool Validation() {
    static const bool value = Detail::Env("NXEMU_METAL_VALIDATION", false);
    return value;
}

/// Keeps compiled pipelines in a binary archive between runs.
inline bool PipelineArchive() {
    static const bool value = Detail::Env("NXEMU_METAL_PIPELINE_ARCHIVE", true);
    return value;
}

/// Runs geometry shaders as compute shaders; Metal has no geometry stage.
inline bool GeometryEmulation() {
    static const bool value = Detail::Env("NXEMU_METAL_GEOMETRY_EMULATION", true);
    return value;
}

/// Runs GPU-driven indirect compute dispatches from the guest's indirect buffer.
inline bool IndirectCompute() {
    static const bool value = Detail::Env("NXEMU_METAL_INDIRECT_COMPUTE", true);
    return value;
}

/// Emulates transform feedback by capturing vertex outputs to buffers.
inline bool TransformFeedback() {
    static const bool value = Detail::Env("NXEMU_METAL_TRANSFORM_FEEDBACK", true);
    return value;
}

} // namespace Metal::Options
