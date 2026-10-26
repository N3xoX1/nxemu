// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "dialog.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <nxemu-module-spec/video.h>
#include <thread>
#include "yuzu_video_core/shader_profile_versions.h"
class UiWindow;
class ShaderProfileDialog final : public Dialog, public IShaderProfileProgress
{
public:
    ShaderProfileDialog(UiWindow & window, const std::filesystem::path & assets, std::function<void(const std::string &)> start);
    ~ShaderProfileDialog();
    void Prepare(IVideo & video, bool importing, std::string game, uint64_t title, std::filesystem::path cache, std::string label);
    bool Busy() const
    {
        return m_busy.load();
    }
    void Cancel()
    {
        m_cancel = true;
    }
    void CancelAndWait();

private:
    // Export: choose what to share, set the options, export, then save the files.
    enum class Step { Mode, Options, Running, Save, Done };
    // One exported file: consecutive recorded versions, optionally completing the installed profile.
    struct ExportOutput {
        std::filesystem::path staged;
        std::vector<size_t> versions;
        bool installed{};
        std::string name;
        ShaderProfileResult result{};
    };
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;
    bool ProfileProgress(const char * phase, uint64_t done, uint64_t total) override;
    void ShowStep(Step step);
    void UpdateGrouping();
    void Start();
    void RunImport();
    void RunExport(std::vector<size_t> selected);
    bool SaveOutputs();
    void RemoveStaging();
    std::string GroupLabel(const std::vector<size_t> & versions) const;
    UiWindow & m_window;
    IVideo * m_video{};
    const std::function<void(const std::string &)> m_startGame;
    std::string m_game, m_gameLabel, m_profile, m_root, m_cacheFile;
    std::filesystem::path m_cache;
    uint64_t m_title{};
    bool m_importing{};
    Step m_step{Step::Options};
    // Recorded versions, oldest first, and the shared profile installed for this title.
    std::vector<VideoCommon::ShaderProfile::RecordingVersion> m_versions;
    size_t m_unavailableVersions{};
    std::filesystem::path m_installed;
    std::string m_installedInfo;
    ShaderProfileResult m_installedResult{};
    // Pipelines shared by each recorded version and the one before it, in percent.
    std::vector<unsigned> m_shares;
    bool m_complete{};
    std::string m_groupingFor;
    // Export results, guarded by m_mutex while the worker runs.
    std::vector<ExportOutput> m_outputs;
    std::vector<std::string> m_exportNotes;
    std::string m_saved;
    std::string m_revision, m_note;
    bool m_fullGame{};
    bool m_finished{}, m_success{};
    std::atomic<bool> m_busy{}, m_cancel{};
    std::thread m_thread;
    std::mutex m_mutex;
    std::string m_phase, m_error, m_currentLabel;
    uint64_t m_done{}, m_total{};
    uint64_t m_foundPrograms{}, m_totalPrograms{};
    std::chrono::steady_clock::time_point m_started;
    ShaderProfileResult m_result{};
};
