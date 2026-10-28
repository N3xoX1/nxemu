// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace VideoCommon::ShaderProfile {
// Call only after confirmation and successful export to a sibling file.
inline void ReplaceCompletedExport(const std::filesystem::path & completed, const std::filesystem::path & output) {
    if (completed == output || completed.parent_path() != output.parent_path() || !std::filesystem::is_regular_file(completed))
        throw std::runtime_error("Invalid completed shader export");
#ifdef _WIN32
    if (!MoveFileExW(completed.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace shader profile; the previous file was preserved");
#else
    std::filesystem::rename(completed, output);
#endif
}
}
