#pragma once
#include "dialog.h"
#include <cstdint>
#include <functional>
#include <nxemu/ui/library/nx_tool.h>
#include <string>
#include <vector>

class UiWindow;

// Converts decrypted DXCI/DNSP files to NX. Files are grouped by game: the
// base, its updates and its DLC go into one .nx per game.
class NxConvertDialog final : public Dialog
{
public:
    struct Host
    {
        std::function<bool()> canWork;      // no game running or starting
        std::function<void()> libraryChanged; // new .nx files were written
    };

    NxConvertDialog(UiWindow & window, const std::filesystem::path & assetDir, Host host);

    bool Busy() const { return m_tool.Busy(); }
    void Cancel();
    // Opens the dialog with these files or folders. With a focus file, only
    // the files of that file's game are kept (its folder is read for the rest).
    void Offer(std::vector<std::string> paths, std::string focus = {});
    bool OfferDrop(const std::string & utf8Path);

private:
    struct Content
    {
        std::string kind; // base, update, dlc
        uint64_t titleId = 0;
        uint64_t applicationId = 0;
        uint32_t version = 0;
        std::string displayVersion;
        std::string name;
    };
    struct Source
    {
        std::string path;
        uint64_t size = 0;
        bool supported = false;
        bool selected = true;
        std::string issue;
        std::vector<Content> contents;
    };
    struct Game
    {
        uint64_t applicationId = 0;
        std::string name;
        std::vector<size_t> sources;
        std::string output; // empty when it cannot be converted
        std::string problem;
    };
    struct Job
    {
        std::string name;
        std::string output;
        std::vector<std::string> sources;
        uint64_t sourceBytes = 0;
        bool done = false;
        bool succeeded = false;
        std::string message;
    };
    enum class Step
    {
        Pick,
        Work,
        Done,
    };

    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void Queue(const std::vector<std::string> & paths);
    void ScanFinished(const NxTool::State & state);
    std::vector<Game> Games() const;
    void ShowStep(Step step);
    void ShowSources();
    void ShowOptions();
    void StartConversion();
    void NextJob();
    void ShowWork(const NxTool::State & state);
    void ShowResults();

    UiWindow & m_window;
    const Host m_host;
    NxTool m_tool;
    Step m_step = Step::Pick;
    std::vector<std::string> m_pendingScan; // waiting for the tool
    std::vector<std::string> m_scanning;    // given to the running scan
    std::string m_focus;
    std::vector<Source> m_sources;
    std::vector<Job> m_jobs;
    size_t m_job = 0;
    bool m_converting = false;
    bool m_cancelled = false;
    bool m_recipe = true;
    int m_level = 3;
    std::string m_outputDir; // empty: beside each game's files
    bool m_changed = false;
    std::string m_lastProgress;
};
