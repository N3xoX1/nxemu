#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <nxemu-module-spec/base.h>
#include <string>
#include <thread>
#include <vector>

class FirmwareAvatars final
{
public:
    FirmwareAvatars() = default;
    ~FirmwareAvatars();

    void Load(ISystemloader & loader);
    bool Loading() const { return m_loading; }
    void Wait();
    std::vector<std::string> Files();

    static void Forget();
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
