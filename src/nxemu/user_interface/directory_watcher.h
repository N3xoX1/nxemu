#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

class DirectoryWatcher
{
public:
    using ChangeCallback = std::function<void()>;

    DirectoryWatcher();
    ~DirectoryWatcher();

    void Start(const std::vector<std::string> & dirs, const std::vector<std::string> & recursiveDirs, ChangeCallback callback);
    void Stop();

private:
    DirectoryWatcher(const DirectoryWatcher &) = delete;
    DirectoryWatcher & operator=(const DirectoryWatcher &) = delete;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
