// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <climits>
#include <thread>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h> // For OutputDebugStringW
#endif

#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/fs_paths.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_common/thread.h"

#include "yuzu_common/logging/backend.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/logging/log_entry.h"
#include "yuzu_common/logging/text_formatter.h"
#include "yuzu_common/settings.h"
#ifdef _WIN32
#include "yuzu_common/string_util.h"
#endif
#include "yuzu_common/bounded_threadsafe_queue.h"

namespace Common::Log
{

namespace
{

/**
 * Interface for logging backends.
 */
class Backend
{
public:
    virtual ~Backend() = default;

    virtual void Write(const Entry & entry) = 0;

    virtual void EnableForStacktrace() = 0;

    virtual void Flush() = 0;
};

/**
 * Backend that writes to stderr and with color
 */
class ColorConsoleBackend final : public Backend
{
public:
    explicit ColorConsoleBackend() = default;

    ~ColorConsoleBackend() override = default;

    void Write(const Entry & entry) override
    {
        if (enabled.load(std::memory_order_relaxed))
        {
            PrintColoredMessage(entry);
        }
    }

    void Flush() override
    {
        // stderr shouldn't be buffered
    }

    void EnableForStacktrace() override
    {
        enabled = true;
    }

    void SetEnabled(bool enabled_)
    {
        enabled = enabled_;
    }

private:
    std::atomic_bool enabled{false};
};

/**
 * Backend that writes to a file passed into the constructor
 */
class FileBackend final : public Backend
{
public:
    explicit FileBackend(const std::filesystem::path & filename)
    {
        auto old_filename = filename;
        old_filename += ".old.txt";

        // Existence checks are done within the functions themselves.
        // We don't particularly care if these succeed or not.
        static_cast<void>(FS::RemoveFile(old_filename));
        static_cast<void>(FS::RenameFile(filename, old_filename));

        file = std::make_unique<FS::IOFile>(filename, FS::FileAccessMode::Write,
                                            FS::FileType::TextFile);
    }

    ~FileBackend() override = default;

    void Write(const Entry & entry) override
    {
        if (!enabled)
        {
            return;
        }

        bytes_written += file->WriteString(FormatLogMessage(entry).append(1, '\n'));

        using namespace Common::Literals;
        // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
        const auto write_limit = Settings::values.extended_logging.GetValue() ? 1_GiB : 100_MiB;
        const bool write_limit_exceeded = bytes_written > write_limit;
        if (entry.log_level >= LogLevel::Error || write_limit_exceeded)
        {
            if (write_limit_exceeded)
            {
                // Stop writing after the write limit is exceeded.
                // Don't close the file so we can print a stacktrace if necessary
                enabled = false;
            }
            file->Flush();
        }
    }

    void Flush() override
    {
        file->Flush();
    }

    void EnableForStacktrace() override
    {
        enabled = true;
        bytes_written = 0;
    }

private:
    std::unique_ptr<FS::IOFile> file;
    bool enabled = true;
    std::size_t bytes_written = 0;
};

/**
 * Backend that writes to Visual Studio's output window
 */
class DebuggerBackend final : public Backend
{
public:
    explicit DebuggerBackend() = default;

    ~DebuggerBackend() override = default;

    void Write(const Entry & entry) override
    {
#ifdef _WIN32
        ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry).append(1, '\n')).c_str());
#endif
    }

    void Flush() override
    {
    }

    void EnableForStacktrace() override
    {
    }
};

#ifdef ANDROID
/**
 * Backend that writes to the Android logcat
 */
class LogcatBackend : public Backend
{
public:
    explicit LogcatBackend() = default;

    ~LogcatBackend() override = default;

    void Write(const Entry & entry) override
    {
        PrintMessageToLogcat(entry);
    }

    void Flush() override
    {
    }

    void EnableForStacktrace() override
    {
    }
};
#endif

bool initialization_in_progress_suppress_logging = true;

/**
 * Static state as a singleton.
 */
class Impl :
    public IModuleLogger
{
public:
    static Impl & Instance()
    {
        if (!instance)
        {
            throw std::runtime_error("Using Logging instance before its initialization");
        }
        return *instance;
    }

    static void Initialize(IModuleLogger * logger, const char * filterType)
    {
        if (instance)
        {
            LOG_WARNING(Log, "Reinitializing logging backend");
            return;
        }
        Filter filter;
        filter.ParseFilterString(filterType);
        if (logger)
        {
            instance = std::unique_ptr<Impl, decltype(&Deleter)>(new Impl(logger, filter), Deleter);
        }
        else
        {
            using namespace Common::FS;
            const auto & log_dir = GetYuzuPath(YuzuPath::LogDir);
            void(CreateDir(log_dir));
            instance = std::unique_ptr<Impl, decltype(&Deleter)>(new Impl(log_dir / LOG_FILE, filter),
                                                                 Deleter);
        }
        initialization_in_progress_suppress_logging = false;
    }

    static void Start()
    {
        if (instance)
        {
            instance->StartBackendThread();
        }
    }

    static void Stop()
    {
        if (instance)
        {
            instance->StopBackendThread();
        }
    }

    Impl(const Impl &) = delete;
    Impl & operator=(const Impl &) = delete;

    Impl(Impl &&) = delete;
    Impl & operator=(Impl &&) = delete;

    static void ResetFilter(const char * filterType)
    {
        instance->filter.ParseFilterString(filterType);
    }

    void SetGlobalFilter(const Filter & f)
    {
        filter = f;
    }

    void SetColorConsoleBackendEnabled(bool enabled)
    {
        color_console_backend.SetEnabled(enabled);
    }

    void Log(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_num, const char * function, const char * message)
    {
        if (modulelogger != nullptr)
        {
            return;
        }
        if (!filter.CheckMessage(log_class, log_level))
        {
            return;
        }
        message_queue.EmplaceWait(CreateEntry(log_class, log_level, filename, line_num, function, std::string(message)));
    }

    void PushEntry(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_num,
                   const char * function, std::string && message)
    {
        if (modulelogger != nullptr)
        {
            modulelogger->Log(log_class, log_level, filename, line_num, function, message.c_str());
        }
        else
        {
            if (!filter.CheckMessage(log_class, log_level))
            {
                return;
            }
            message_queue.EmplaceWait(CreateEntry(log_class, log_level, filename, line_num, function, std::move(message)));
        }
    }

private:
    Impl(const std::filesystem::path & file_backend_filename, const Filter & filter_) :
        modulelogger(nullptr), filter{filter_}, file_backend{new FileBackend(file_backend_filename)}
    {
    }

    Impl(IModuleLogger * logger, const Filter & filter_) :
        modulelogger{logger}, filter{filter_}, file_backend{nullptr}
    {
    }

    ~Impl() = default;

    void StartBackendThread()
    {
        backend_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("Logger");
            Entry entry;
            const auto write_logs = [this, &entry]() {
                ForEachBackend([&entry](Backend & backend) { backend.Write(entry); });
            };
            while (!stop_token.stop_requested())
            {
                message_queue.PopWait(entry, stop_token);
                if (stop_token.stop_requested())
                {
                    break;
                }
                write_logs();
            }

            // Drain remaining entries in the queue
            int max_logs_to_write = filter.IsDebug() ? INT_MAX : 100;
            while (max_logs_to_write-- && message_queue.TryPop(entry))
            {
                write_logs();
            }
        });
    }

    void StopBackendThread()
    {
        backend_thread.request_stop();
        if (backend_thread.joinable())
        {
            backend_thread.join();
        }

        ForEachBackend([](Backend & backend) { backend.Flush(); });
    }

    Entry CreateEntry(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_nr,
                      const char * function, std::string && message) const
    {
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        using std::chrono::steady_clock;

        return {
            .timestamp = duration_cast<microseconds>(steady_clock::now() - time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = filename,
            .line_num = line_nr,
            .function = function,
            .message = std::move(message),
        };
    }

    void ForEachBackend(auto lambda)
    {
        lambda(static_cast<Backend &>(debugger_backend));
        lambda(static_cast<Backend &>(color_console_backend));
        if (file_backend != nullptr)
        {
            lambda(static_cast<Backend &>(*file_backend));
        }
#ifdef ANDROID
        lambda(static_cast<Backend &>(lc_backend));
#endif
    }

    static void Deleter(Impl * ptr)
    {
        delete ptr;
    }

    static inline std::unique_ptr<Impl, decltype(&Deleter)> instance{nullptr, Deleter};

    Filter filter;
    IModuleLogger * modulelogger;
    DebuggerBackend debugger_backend{};
    ColorConsoleBackend color_console_backend{};
    std::unique_ptr<FileBackend> file_backend;
#ifdef ANDROID
    LogcatBackend lc_backend{};
#endif

    MPSCQueue<Entry> message_queue{};
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};
    std::jthread backend_thread;
};
} // namespace

void Initialize(IModuleLogger * logger, const char * filterType)
{
    Impl::Initialize(logger, filterType);
}

void Start()
{
    Impl::Start();
}

void Stop()
{
    Impl::Stop();
}

void ResetFilter(const char * filterType)
{
    Impl::ResetFilter(filterType);
}

void DisableLoggingInTests()
{
    initialization_in_progress_suppress_logging = true;
}

void SetGlobalFilter(const Filter & filter)
{
    Impl::Instance().SetGlobalFilter(filter);
}

void SetColorConsoleBackendEnabled(bool enabled)
{
    Impl::Instance().SetColorConsoleBackendEnabled(enabled);
}

void FmtLogMessageImpl(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_num, const char * function, const char * format, const fmt::format_args & args)
{
    if (initialization_in_progress_suppress_logging)
    {
        return;
    }
    Impl::Instance().PushEntry(log_class, log_level, filename, line_num, function, fmt::vformat(format, args));
}

IModuleLogger * ModuleLogger()
{
    return &Impl::Instance();
}

} // namespace Common::Log
