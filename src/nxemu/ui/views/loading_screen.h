#pragma once
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

class UiRenderer;
nxinterface IRomInfo;
namespace Rml
{
class Element;
}

class LoadingScreen
{
public:
    LoadingScreen(Rml::Element * panel, UiRenderer & renderer);
    ~LoadingScreen();

    void Reset();
    bool HasGame() const { return m_hasGame; }
    void Announce(const std::string & title, const std::string & iconSource);
    void SetGame(IRomInfo & info);
    void Update();

private:
    LoadingScreen(const LoadingScreen &) = delete;
    LoadingScreen & operator=(const LoadingScreen &) = delete;

    void SetImage(const char * id, std::vector<uint8_t> encoded);
    void SetText(const char * id, const std::string & text);
    void SetStatus(const std::string & text, const std::string & count = {});
    void SetProgress(int percent);

    Rml::Element * m_panel;
    UiRenderer & m_renderer;
    std::vector<std::string> m_images;
    std::string m_title;
    int m_announcedTick = 0;
    SettingsStore::DiskCacheProgress m_progress;
    bool m_hasGame = false;
};
