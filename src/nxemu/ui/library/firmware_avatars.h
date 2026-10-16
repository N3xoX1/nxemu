#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <nxemu-module-spec/base.h>
#include <string>
#include <thread>
#include <vector>

// The pictures the console offers its users for their profile, which come
// with its firmware. They are read from it once, on a thread of their own,
// and kept as files: a picture is ready when its file is listed.
class FirmwareAvatars final
{
public:
    FirmwareAvatars() = default;
    ~FirmwareAvatars();

    // Starts reading the pictures, unless they are read or being read.
    void Load(ISystemloader & loader);
    bool Loading() const { return m_loading; }
    // Waits for the reading to end: the loader is about to go.
    void Wait();
    // The pictures read so far, as PNG files with a transparent background.
    std::vector<std::string> Files();

    // The firmware changed: the pictures kept are no longer its own.
    static void Forget();
    // A picture on a background of the colour, as a PNG file in memory.
    // Empty when the file is not a picture of the console.
    static std::vector<uint8_t> OnColour(const std::string & file, uint8_t red, uint8_t green, uint8_t blue);

private:
    FirmwareAvatars(const FirmwareAvatars &) = delete;
    FirmwareAvatars & operator=(const FirmwareAvatars &) = delete;

    void Read(ISystemloader & loader);

    std::thread m_thread;
    std::atomic_bool m_loading{false};
    std::mutex m_mutex;
    std::vector<std::string> m_files;
};
