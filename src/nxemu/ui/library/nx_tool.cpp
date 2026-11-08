#include "nx_tool.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
    const int code = nxformat_run(argv.data(), argv.size(), onLine, this, handle, error, sizeof(error));
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
