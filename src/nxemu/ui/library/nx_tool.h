#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Runs an nxconvert command of the nxformat library on a worker thread.
// Machine lines it reports start with '@' and hold tab-separated fields;
// "@TOTAL" (added up) and "@PROGRESS" become the progress. Cancelling stops the command
// at its next check: it removes its temporary file and leaves the archive as
// it was.
class NxTool
{
public:
    enum class Phase
    {
        Idle,
        Running,
        Done,
    };

    struct State
    {
        Phase phase = Phase::Idle;
        uint64_t total = 0;
        uint64_t progress = 0;
        std::chrono::steady_clock::time_point started;
        bool succeeded = false;
        bool cancelled = false;
        std::string error;
        std::vector<std::vector<std::string>> records; // fields of each '@' line, without '@'
    };

    NxTool() = default;
    ~NxTool();

    // The command and its arguments, as for nxconvert. Fails when busy.
    bool Start(std::vector<std::string> arguments);
    State Current() const;
    State TakeState(); // take the newly published scan records
    bool Busy() const;
    void Cancel();
    void Reset();

private:
    NxTool(const NxTool &) = delete;
    NxTool & operator=(const NxTool &) = delete;

    void Run(std::vector<std::string> arguments);
    void Line(const char * line);

    mutable std::mutex m_mutex;
    State m_state;
    bool m_cancel = false;
    struct nxformat_cancel * m_handle = nullptr; // set while a command runs
    std::thread m_thread;
};

bool CanRetryWithoutRecipe(const NxTool::State & state);
std::vector<std::string> SplitFields(const std::string & line);
// The progress of a running command: percent done and time elapsed, or what it is doing
// before and after the bytes it counts.
std::string ProgressText(const NxTool::State & state);
