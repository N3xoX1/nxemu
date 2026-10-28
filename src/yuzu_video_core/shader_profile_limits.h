// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>

namespace VideoCommon::ShaderProfile {
// Sized for large titles with several updates. Mario Kart 8 Deluxe with two
// updates records about 19k observations per version and 7k guest programs.
// Recipes recorded for one game version, and recipes in one shared profile.
inline constexpr size_t MaxRecipes = 131072;
// A shared .nxshader file, and its decompressed payload.
inline constexpr size_t MaxProfilePayloadBytes = 128 * 1024 * 1024;
inline constexpr size_t MaxProfileFileBytes = MaxProfilePayloadBytes + 1024 * 1024;
// The local pipeline cache shared by every recorded version of a title.
inline constexpr size_t MaxLocalCacheBytes = size_t{1024} * 1024 * 1024;
inline constexpr size_t MaxLocalCacheObjects = 1000000;
// Pipelines associated with one game version in the local cache.
inline constexpr size_t MaxVersionPipelines = 262144;
} // namespace VideoCommon::ShaderProfile
