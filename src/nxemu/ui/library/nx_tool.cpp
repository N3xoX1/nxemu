#include "nx_tool.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <set>
#include <yuzu_common/fs/fs_util.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxformat.h>

std::vector<std::string> SplitFields(const std::string & line)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true)
    {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
        {
            return fields;
        }
        start = tab + 1;
    }
}

bool CanRetryWithoutRecipe(const NxTool::State & state)
{
    if (state.succeeded || state.cancelled)
    {
        return false;
    }
    std::string error = state.error;
    std::transform(error.begin(), error.end(), error.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return error.find("recipe") != std::string::npos || error.find("rebuild") != std::string::npos;
}

std::string ProgressText(const NxTool::State & state)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - state.started).count();
    char elapsed[32];
    if (seconds >= 3600)
    {
        std::snprintf(elapsed, sizeof(elapsed), "%lld:%02lld:%02lld", (long long)(seconds / 3600), (long long)(seconds / 60 % 60), (long long)(seconds % 60));
    }
    else
    {
        std::snprintf(elapsed, sizeof(elapsed), "%lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
    }
    if (state.total == 0)
    {
        return Tr("Preparing... (%1)", {elapsed});
    }
    if (state.progress >= state.total)
    {
        return Tr("Finishing... (%1)", {elapsed});
    }
    // It stays below 100 % until the command has returned.
    const uint64_t percent = std::min<uint64_t>(99, state.progress * 100 / state.total);
    return Tr("%1 % done (%2)", {std::to_string(percent), elapsed});
}

NxTool::~NxTool()
{
    Cancel();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool NxTool::Start(std::vector<std::string> arguments)
{
    if (Busy())
    {
        return false;
    }
    Reset();
    arguments.push_back("--frontend");
    {
        std::lock_guard lock(m_mutex);
        m_state.phase = Phase::Running;
        m_state.started = std::chrono::steady_clock::now();
    }
    m_thread = std::thread([this, arguments = std::move(arguments)]() mutable { Run(std::move(arguments)); });
    return true;
}

NxTool::State NxTool::Current() const
{
    std::lock_guard lock(m_mutex);
    return m_state;
}

NxTool::State NxTool::TakeState()
{
    std::lock_guard lock(m_mutex);
    auto records = std::move(m_state.records);
    m_state.records.clear();
    State state = m_state;
    state.records = std::move(records);
    return state;
}

bool NxTool::Busy() const
{
    std::lock_guard lock(m_mutex);
    return m_state.phase == Phase::Running;
}

void NxTool::Cancel()
{
    std::lock_guard lock(m_mutex);
    m_cancel = true;
    if (m_handle != nullptr)
    {
        nxformat_cancel_set(m_handle);
    }
}

void NxTool::Reset()
{
    if (Busy())
    {
        return;
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    std::lock_guard lock(m_mutex);
    m_state = {};
    m_cancel = false;
}

void NxTool::Line(const char * text)
{
    if (text == nullptr || text[0] != '@')
    {
        return;
    }
    std::vector<std::string> fields = SplitFields(text + 1);
    std::lock_guard lock(m_mutex);
    if (fields[0] == "PROGRESS" && fields.size() > 1)
    {
        m_state.progress = std::strtoull(fields[1].c_str(), nullptr, 10);
    }
    else if (fields[0] == "TOTAL" && fields.size() > 1)
    {
        // Each line adds the bytes of one more step.
        m_state.total += std::strtoull(fields[1].c_str(), nullptr, 10);
    }
    else
    {
        m_state.records.push_back(std::move(fields));
    }
}

void NxTool::Run(std::vector<std::string> arguments)
{
    std::vector<const char *> argv;
    argv.reserve(arguments.size());
    for (const std::string & argument : arguments)
    {
        argv.push_back(argument.c_str());
    }
    nxformat_cancel * handle = nxformat_cancel_new();
    {
        std::lock_guard lock(m_mutex);
        m_handle = handle;
        if (m_cancel)
        {
            nxformat_cancel_set(handle);
        }
    }
    const nxformat_line_fn onLine = [](void * user, int isError, const char * line) {
        if (isError == 0)
        {
            static_cast<NxTool *>(user)->Line(line);
        }
    };
    char error[1024] = {};
    int code;
    if (arguments.front() == "scan")
    {
        // Scan each package independently. A batch inventory can read entire
        // payloads to disambiguate duplicates, which is only needed at pack time.
        using Records = std::vector<std::vector<std::string>>;
        const auto collect = [](void * user, int isError, const char * line) {
            if (isError == 0 && line != nullptr && line[0] == '@')
            {
                static_cast<Records *>(user)->push_back(SplitFields(line + 1));
            }
        };
        std::set<std::filesystem::path> seen;
        const auto scan = [&](const std::filesystem::path & path) {
            if (!seen.insert(path.lexically_normal()).second)
            {
                return;
            }
            const std::string name = Common::FS::PathToUTF8String(path);
            const char * input[] = {"scan", name.c_str(), "--frontend"};
            Records records;
            char failure[1024] = {};
            const int result = nxformat_run(input, 3, collect, &records, handle, failure, sizeof(failure));
            if (result == NXFORMAT_CANCELLED)
            {
                return;
            }
            if (records.empty())
            {
                records.push_back({"SOURCE", name, "0", "", "Rejected"});
            }
            if (result != NXFORMAT_OK && failure[0] != '\0')
            {
                records.push_back({"ISSUE", name, failure});
            }
            std::lock_guard lock(m_mutex);
            m_state.records.insert(m_state.records.end(), std::make_move_iterator(records.begin()), std::make_move_iterator(records.end()));
        };
        const auto cancelled = [this] {
            std::lock_guard lock(m_mutex);
            return m_cancel;
        };
        for (size_t i = 1; i + 1 < arguments.size() && !cancelled(); ++i)
        {
            const std::filesystem::path path{Common::FS::ToU8String(arguments[i])};
            std::error_code ec;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(path, ec)))
            {
                scan(path);
                continue;
            }
            std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
            for (; it != end && !ec && !cancelled(); it.increment(ec))
            {
                if (it.depth() >= 7)
                {
                    it.disable_recursion_pending();
                }
                auto ext = Common::FS::PathToUTF8String(it->path().extension());
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                std::error_code statusError;
                if ((ext == ".dxci" || ext == ".dnsp" || ext == ".xci" || ext == ".nsp") &&
                    std::filesystem::is_regular_file(it->symlink_status(statusError)))
                {
                    scan(it->path());
                }
            }
        }
        code = NXFORMAT_OK;
    }
    else
    {
        code = nxformat_run(argv.data(), argv.size(), onLine, this, handle, error, sizeof(error));
    }
    std::lock_guard lock(m_mutex);
    m_handle = nullptr;
    nxformat_cancel_free(handle);
    m_state.phase = Phase::Done;
    m_state.cancelled = m_cancel || code == NXFORMAT_CANCELLED;
    m_state.succeeded = code == NXFORMAT_OK && !m_state.cancelled;
    if (!m_state.succeeded && !m_state.cancelled)
    {
        m_state.error = error[0] != '\0' ? error : "the NX tool stopped with an error";
    }
}
