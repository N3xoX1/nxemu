// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile_provenance.h"

namespace VideoCommon::ShaderProfile {
// State of the shared profile selected for a title (see shader_profile_context.h).
inline std::filesystem::path ImportSelectionPath(const std::filesystem::path& cache) { return ImportedDirectory(cache) / "selection.json"; }
inline std::filesystem::path ImportIntegratedPath(const std::filesystem::path& cache) { return ImportedDirectory(cache) / "integrated.json"; }
inline std::filesystem::path ImportAttemptedPath(const std::filesystem::path& cache) { return ImportedDirectory(cache) / "attempted.json"; }
inline std::filesystem::path ImportStatusPath(const std::filesystem::path& cache) { return ImportedDirectory(cache) / "status.json"; }
inline std::filesystem::path ImportPendingPath(const std::filesystem::path& cache) { return ImportedDirectory(cache) / "pending.nxshader"; }
inline std::filesystem::path WarmupPath(const std::filesystem::path& cache) { return cache / "warmup.json"; }
} // namespace VideoCommon::ShaderProfile
