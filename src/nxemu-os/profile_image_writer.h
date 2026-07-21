#pragma once

#include <stdint.h>
#include <filesystem>

bool WriteProfileJpegFromMemory(const uint8_t * data, size_t size, const std::filesystem::path & destination);
