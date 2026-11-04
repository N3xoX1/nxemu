#pragma once
#include "dialog.h"

class EmulatorSession;

class AboutDialog final : public Dialog
{
public:
    AboutDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir);

private:
    void OnOpen() override;
    void OnCommand(const std::string & command) override;

    EmulatorSession & m_session;
    int m_versionClicks = 0;
};
