#include "atomic_file.h"
#include <chrono>
#include <fstream>
#include <functional>
#include <thread>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

bool Common::WriteFileAtomically(const std::filesystem::path & path, const char * data, size_t size)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::filesystem::path temporary = path;
    temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(data, static_cast<std::streamsize>(size));
        file.close();
        if (!file)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
#ifdef _WIN32
    const bool replaced = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!replaced) { std::filesystem::remove(temporary, error); return false; }
#else
    std::filesystem::rename(temporary, path, error);
    if (error) { std::filesystem::remove(temporary, error); return false; }
#endif
    return true;
}

