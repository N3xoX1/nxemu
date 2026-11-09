#pragma once
#include "dialog.h"
#include <cstdint>
#include <functional>
#include <nxemu/ui/library/nx_tool.h>
#include <string>
#include <vector>

class UiWindow;

// Shows what an .nx file holds (the game, its updates and DLC) and adds or
// removes updates and DLC in place. The game itself is never rewritten. The
// rebuild recipes kept inside the file recreate the original DXCI or DNSP.
class NxContentsDialog final : public Dialog
{
public:
    struct Pictures
    {
        std::vector<uint8_t> current;
        std::vector<uint8_t> original;
    };
    struct Host
    {
        std::function<bool()> canWork;        // no game running or starting
        std::function<void()> libraryChanged; // the file changed
        std::function<Pictures(const std::string &)> readPictures;
    };

    NxContentsDialog(UiWindow & window, const std::filesystem::path & assetDir, Host host);

    bool Busy() const { return m_tool.Busy(); }
    void Cancel() { m_tool.Cancel(); }
    // With `editTitle`, the title and picture page opens once the file is read.
    void Open(const std::string & utf8Archive, bool editTitle = false);
    bool OfferDrop(const std::string & utf8Path);

private:
    struct Item
    {
        std::string identity;
        std::string kind; // base, update, dlc
        uint64_t titleId = 0;
        uint32_t version = 0;
        std::string displayVersion;
        std::string name;
    };
    struct Original
    {
        std::string id; // recipe checksum, as nxconvert rebuild takes it
        std::string name;
        uint64_t size = 0;
    };
    enum class Task
    {
        None,
        List,
        Add,
        Remove,
        Rebuild,
        Metadata,
    };

    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void Run(Task task, std::vector<std::string> arguments, std::string what);
    void Finished(const NxTool::State & state);
    void Add(std::vector<std::string> files, bool recipe = true);
    void ShowList();
    void ShowPage(const char * page);
    void ShowEdit();
    void ChoosePicture();
    void StagePicture(const std::vector<uint8_t> & icon, const std::string & note);
    void ClearEditPictures();
    void SaveMetadata();
    void ForgetPicture();

    UiWindow & m_window;
    const Host m_host;
    NxTool m_tool;
    std::string m_archive;
    std::vector<Item> m_items;
    std::vector<Original> m_originals;
    uint64_t m_size = 0;
    Task m_task = Task::None;
    std::string m_what;
    std::string m_message;
    bool m_messageFailed = false;
    std::vector<std::string> m_addFiles;
    bool m_addRecipe = true;
    bool m_addCommitted = false;
    bool m_retryAdd = false;
    size_t m_asking = 0;
    bool m_changed = false;
    std::string m_shownProgress;
    std::string m_applicationId; // as "contents" lists it, 16 hex digits
    bool m_editOnOpen = false;
    std::string m_catalogTitle;
    std::string m_shownTitle; // the title the edit page started from
    std::string m_currentPicture;
    std::vector<uint8_t> m_originalPicture;
    std::string m_picture; // staged image file, until it is saved
    unsigned m_pictures = 0;
};
