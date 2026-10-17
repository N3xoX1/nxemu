#pragma once
#include <cstddef>
#include <filesystem>

namespace Common
{
// Replace a file in place, using a temporary sibling. Failed writes leave the
// previous file intact. Works when the destination already exists on Windows.
bool WriteFileAtomically(const std::filesystem::path & path, const char * data, size_t size);
}
