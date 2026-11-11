// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "dialog.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <nxemu-module-spec/video.h>
#include <thread>
#include "yuzu_video_core/shader_profile_versions.h"
class UiWindow;
// Exports a game's shaders, every version played or imported, then saves the files.
class ShaderProfileDialog final : public Dialog, public IShaderProfileProgress
{
public:
    ShaderProfileDialog(UiWindow & window, const std::filesystem::path & assets);
    ~ShaderProfileDialog();
    void Prepare(IVideo & video, std::string game, uint64_t title, std::filesystem::path cache, std::string label);
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
    enum class Step { Running, Save, Done };
    // One exported file: consecutive versions.
    struct ExportOutput {
        std::filesystem::path staged;
        std::vector<size_t> versions;
        std::string name;
    };
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;
    bool ProfileProgress(const char * phase, uint64_t done, uint64_t total) override;
    void ShowStep(Step step);
    void Start();
    void RunExport();
    bool SaveOutputs();
    void RemoveStaging();
    std::string GroupLabel(const std::vector<size_t> & versions) const;
    UiWindow & m_window;
    IVideo * m_video{};
    std::string m_game, m_gameLabel, m_root, m_cacheFile;
    std::filesystem::path m_cache;
    uint64_t m_title{};
    Step m_step{Step::Done};
    // Versions with shaders to export, oldest first.
    std::vector<VideoCommon::ShaderProfile::RecordingVersion> m_versions;
    // Versions with shaders that cannot be identified, so cannot be exported.
    size_t m_unidentified{};
    // Export results, guarded by m_mutex while the worker runs.
    std::vector<ExportOutput> m_outputs;
    std::vector<std::string> m_exportNotes;
    std::string m_saved;
    bool m_finished{}, m_success{};
    std::atomic<bool> m_busy{}, m_cancel{};
    std::thread m_thread;
    std::mutex m_mutex;
    std::string m_phase, m_error, m_currentLabel;
    uint64_t m_done{}, m_total{};
    uint64_t m_foundPrograms{}, m_totalPrograms{};
    std::chrono::steady_clock::time_point m_started;
};
