#pragma once
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

class UiRenderer;
nxinterface IRomInfo;
namespace Rml
{
class Element;
}

// The screen shown from the moment a game is opened until its first frame: the
// game's icon and name, the logos its file carries, and under a progress bar
// what is being done, which is all that changes while the game loads.
class LoadingScreen
{
public:
    LoadingScreen(Rml::Element * panel, UiRenderer & renderer);
    ~LoadingScreen();

    // Starts over for another game.
    void Reset();
    bool HasGame() const { return m_hasGame; }
    // Starts over with what is known of a game before the loader opens it.
    void Announce(const std::string & title, const std::string & iconSource);
    // Shows the game the loader has opened.
    void SetGame(IRomInfo & info);
    // Refreshes what is being done and the progress bar.
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
    int m_announcedTick = 0; // the progress reported when the game was opened: the game before it
    bool m_hasGame = false;
};
