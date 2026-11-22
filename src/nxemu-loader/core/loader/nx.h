#pragma once
#include "core/loader/loader.h"
#include "nxformat/nx_session.hpp"
namespace FileSys { class ContentProvider; }

namespace Loader {
// Native NCA content, or a complete extracted-tree view (exefs/ + romfs/).
// Exact snapshot selection is available to callers;
// the ordinary host route chooses the newest version of the requested Program.
class AppLoader_NX final : public AppLoader {
public:
    AppLoader_NX(FileSys::VirtualFile file, uint64_t program_id=0, size_t program_index=0,
                 std::string_view host_path={}, std::optional<std::array<uint8_t,32>> snapshot={},
                 nx::SessionOptions options={}, std::optional<uint64_t> application_id={});
    ~AppLoader_NX() override;
    struct CatalogMetadata {
        uint64_t program = 0;
        std::vector<uint64_t> programs;
        std::string title, publisher, version;
        std::vector<uint8_t> icon;
    };
    static std::optional<CatalogMetadata> ReadCatalogMetadata(const FileSys::VirtualFile& file);
    static LoaderFileType IdentifyType(const FileSys::VirtualFile& file);
    LoaderFileType GetFileType() const override { return LoaderFileType::NX; }
    LoaderResultStatus GetStatus() const;
    uint32_t SelectedSnapshot() const;
    uint32_t SelectedVersion() const;
    bool UpdateSelected() const;
    uint32_t ControlSnapshot() const;
    std::shared_ptr<nx::Session> Session() const;
    FileSys::VirtualDir ExeFS() const;
    void AddToManualContentProvider(IManualContentProvider& provider) const;
    // The updates kept for the started Program, newest first; `used` marks the
    // one the game starts with.
    struct Update { uint32_t version=0; std::string display; bool used=false; };
    std::vector<Update> Updates() const;
    bool ActiveContent(ActiveProgramContent& out) const;
    FileSys::ContentProvider& ContentProvider(FileSys::ContentProvider& fallback) const;
    LoadResult Load(Systemloader& loader, ISystemModules& modules) override;
    LoaderResultStatus VerifyIntegrity(std::function<bool(size_t,size_t)> progress) override;
    LoaderResultStatus ReadRomFS(FileSys::VirtualFile& out) override;
    LoaderResultStatus ReadProgramId(uint64_t& id) override;
    LoaderResultStatus ReadProgramIds(uint64_t* ids,uint32_t* count) override;
    LoaderResultStatus ReadControlData(FileSys::NACP& nacp) override;
    LoaderResultStatus ReadTitle(char* out,uint32_t* size) override;
    LoaderResultStatus ReadDisplayVersion(char* out,uint32_t* size) const;
    LoaderResultStatus ReadIcon(uint8_t* out,uint32_t* size) override;
    LoaderResultStatus ReadOriginalIcon(uint8_t* out,uint32_t* size) const;
    LoaderResultStatus ReadNSOModules(Modules& modules) override;
    bool IsRomFSUpdatable() const override { return false; }
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
