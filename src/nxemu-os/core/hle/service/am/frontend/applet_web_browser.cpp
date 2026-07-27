// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/frontend/applet_web_browser.h"
#include "applets/web_browser.h"
#include "core/core.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/hle/result.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/am/service/storage.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/ns/platform_service_manager.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/yuzu_assert.h"

namespace Service::AM::Frontend
{

namespace
{

template <typename T>
void ParseRawValue(T & value, const std::vector<u8> & data)
{
    static_assert(std::is_trivially_copyable_v<T>, "It's undefined behavior to use memcpy with non-trivially copyable objects");
    std::memcpy(&value, data.data(), data.size());
}

template <typename T>
T ParseRawValue(const std::vector<u8> & data)
{
    T value;
    ParseRawValue(value, data);
    return value;
}

std::string ParseStringValue(const std::vector<u8> & data)
{
    return Common::StringFromFixedZeroTerminatedBuffer((const char *)data.data(),data.size());
}

std::string GetMainURL(const std::string & url)
{
    const auto index = url.find('?');

    if (index == std::string::npos)
    {
        return url;
    }

    return url.substr(0, index);
}

std::string ResolveURL(const std::string & url)
{
    const auto index = url.find_first_of('%');

    if (index == std::string::npos)
    {
        return url;
    }

    return url.substr(0, index) + "lp1" + url.substr(index + 1);
}

WebArgInputTLVMap ReadWebArgs(const std::vector<u8> & web_arg, WebArgHeader & web_arg_header)
{
    std::memcpy(&web_arg_header, web_arg.data(), sizeof(WebArgHeader));

    if (web_arg.size() == sizeof(WebArgHeader))
    {
        return {};
    }

    WebArgInputTLVMap input_tlv_map;

    u64 current_offset = sizeof(WebArgHeader);

    for (std::size_t i = 0; i < web_arg_header.total_tlv_entries; ++i)
    {
        if (web_arg.size() < current_offset + sizeof(WebArgInputTLV))
        {
            return input_tlv_map;
        }

        WebArgInputTLV input_tlv;
        std::memcpy(&input_tlv, web_arg.data() + current_offset, sizeof(WebArgInputTLV));

        current_offset += sizeof(WebArgInputTLV);

        if (web_arg.size() < current_offset + input_tlv.arg_data_size)
        {
            return input_tlv_map;
        }

        std::vector<u8> data(input_tlv.arg_data_size);
        std::memcpy(data.data(), web_arg.data() + current_offset, input_tlv.arg_data_size);

        current_offset += input_tlv.arg_data_size;

        input_tlv_map.insert_or_assign(input_tlv.input_tlv_type, std::move(data));
    }

    return input_tlv_map;
}

IVirtualFilePtr GetOfflineRomFS(Core::System & system, u64 title_id, LoaderContentRecordType nca_type)
{
    ISystemloader & loader = system.GetSystemloader();

    if (nca_type == LoaderContentRecordType::Data)
    {
        IFileSystemController & fsc = loader.FileSystemController();
        IFileSysRegisteredCache & nand = fsc.GetSystemNANDContents();
        const FileSysNCAPtr nca(nand.GetEntry(title_id, nca_type));
        if (nca)
        {
            IVirtualFilePtr romfs(nca->GetRomFS());
            if (romfs)
            {
                return romfs;
            }
        }

        LOG_ERROR(Service_AM, "Data NCA for title_id={:016X} not in System NAND; using synthesized system archive", title_id);
        return IVirtualFilePtr(loader.SynthesizeSystemArchive(title_id));
    }

    const FileSysNCAPtr nca(loader.GetContentProviderEntry(title_id, nca_type));
    IVirtualFilePtr romfs;
    if (nca)
    {
        romfs = IVirtualFilePtr(nca->GetRomFS());
    }
    if (romfs)
    {
        return romfs;
    }

    if (nca_type == LoaderContentRecordType::HtmlDocument)
    {
        LOG_WARNING(Service_AM, "Falling back to manual RomFS from the loaded application.");
        IRomInfoPtr loaded(loader.LoadedRomInfo());
        if (loaded)
        {
            romfs = IVirtualFilePtr(loaded->ReadManualRomFS());
        }
    }

    if (!romfs)
    {
        LOG_ERROR(Service_AM, "NCA of type={} with title_id={:016X} is not found in the ContentProvider!", nca_type, title_id);
    }
    return romfs;
}

bool CopyVirtualDirectoryTree(IVirtualDirectoryPtr & src, IVirtualDirectoryPtr & dst)
{
    if (!src || !dst)
    {
        return false;
    }

    IVirtualFileListPtr files(src->GetFiles());
    if (files)
    {
        const uint32_t file_count = files->GetSize();
        for (uint32_t i = 0; i < file_count; ++i)
        {
            IVirtualFilePtr src_file(files->GetItem(i));
            if (!src_file)
            {
                continue;
            }
            IVirtualFilePtr dst_file(dst->CreateFile(src_file->GetName()));
            if (!dst_file)
            {
                return false;
            }
            const u64 sz = src_file->GetSize();
            if (!dst_file->Resize(sz))
            {
                return false;
            }
            constexpr u64 k_chunk = 0x100000;
            std::vector<u8> buf(static_cast<std::size_t>(std::min<u64>(k_chunk, std::max<u64>(sz, 1ULL))));
            u64 off = 0;
            while (off < sz)
            {
                const u64 chunk = std::min<u64>(static_cast<u64>(buf.size()), sz - off);
                if (src_file->ReadBytes(buf.data(), chunk, off) != chunk)
                {
                    return false;
                }
                if (dst_file->WriteBytes(buf.data(), chunk, off) != chunk)
                {
                    return false;
                }
                off += chunk;
            }
        }
    }

    IVirtualDirectoryListPtr subs(src->GetSubdirectories());
    if (!subs)
    {
        return true;
    }

    const uint32_t dir_count = subs->GetSize();
    for (uint32_t i = 0; i < dir_count; ++i)
    {
        IVirtualDirectoryPtr sub_src(subs->GetItem(i));
        if (!sub_src)
        {
            continue;
        }
        IVirtualDirectoryPtr sub_dst(dst->CreateSubdirectory(sub_src->GetName()));
        if (!sub_dst)
        {
            return false;
        }
        if (!CopyVirtualDirectoryTree(sub_src, sub_dst))
        {
            return false;
        }
    }
    return true;
}

void ExtractSharedFonts(Core::System & system)
{
    static constexpr std::array<const char *, 7> DECRYPTED_SHARED_FONTS{
        "FontStandard.ttf",
        "FontChineseSimplified.ttf",
        "FontExtendedChineseSimplified.ttf",
        "FontChineseTraditional.ttf",
        "FontKorean.ttf",
        "FontNintendoExtended.ttf",
        "FontNintendoExtended2.ttf",
    };

    const auto fonts_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / "fonts";

    for (std::size_t i = 0; i < NS::SHARED_FONTS.size(); ++i)
    {
        const auto font_file_path = fonts_dir / DECRYPTED_SHARED_FONTS[i];

        if (Common::FS::Exists(font_file_path))
        {
            continue;
        }

        const auto font = NS::SHARED_FONTS[i];
        const auto font_title_id = static_cast<u64>(font.first);

        ISystemloader & loader = system.GetSystemloader();
        IFileSystemController & fsc = loader.FileSystemController();
        IFileSysRegisteredCache & nand = fsc.GetSystemNANDContents();
        const FileSysNCAPtr nca(nand.GetEntry(font_title_id, LoaderContentRecordType::Data));

        IVirtualFilePtr romfs = IVirtualFilePtr(!nca ? loader.SynthesizeSystemArchive(font_title_id) : nca->GetRomFS());
        if (!romfs)
        {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} cannot be extracted!", font_title_id);
            continue;
        }

        IVirtualDirectoryPtr extracted_romfs = romfs->ExtractRomFS();

        if (!extracted_romfs)
        {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} failed to extract!", font_title_id);
            continue;
        }
        const IVirtualFilePtr font_file(extracted_romfs->GetFile(font.second));

        if (!font_file)
        {
            LOG_ERROR(Service_AM, "SharedFont RomFS with title_id={:016X} has no font file \"{}\"!", font_title_id, font.second);
            continue;
        }

        const std::vector<uint8_t> font_bytes = font_file.ReadAllBytes();
        if (font_bytes.empty() || (font_bytes.size() % sizeof(u32)) != 0)
        {
            LOG_ERROR(Service_AM, "SharedFont {:016X} file \"{}\" has invalid size {} (must be multiple of 4)", font_title_id, font.second, font_bytes.size());
            continue;
        }

        std::vector<u32> font_data_u32(font_bytes.size() / sizeof(u32));
        std::memcpy(font_data_u32.data(), font_bytes.data(), font_bytes.size());
        std::transform(font_data_u32.begin(), font_data_u32.end(), font_data_u32.begin(), Common::swap32);

        std::vector<u8> decrypted_data(font_file->GetSize() - 8);
        NS::DecryptSharedFontToTTF(font_data_u32, decrypted_data);

        IVirtualFilePtr decrypted_font(loader.CreateMemoryFile(decrypted_data.data(), decrypted_data.size(), DECRYPTED_SHARED_FONTS[i]));
        if (!decrypted_font)
        {
            LOG_ERROR(Service_AM, "CreateMemoryFile failed for {}", DECRYPTED_SHARED_FONTS[i]);
            continue;
        }

        const std::string fonts_path_utf8 = Common::FS::PathToUTF8String(fonts_dir);
        IVirtualDirectoryPtr temp_dir(loader.Filesystem().CreateDirectory(fonts_path_utf8.c_str(), VirtualFileOpenMode::ReadWrite));
        if (!temp_dir)
        {
            LOG_ERROR(Service_AM, "Could not create shared-font cache directory {}", fonts_path_utf8);
            continue;
        }

        IVirtualFilePtr out_file(temp_dir->CreateFile(DECRYPTED_SHARED_FONTS[i]));
        if (!out_file)
        {
            LOG_ERROR(Service_AM, "Could not create {}", DECRYPTED_SHARED_FONTS[i]);
            continue;
        }

        constexpr uint64_t k_chunk = 0x100000;
        const uint64_t total_size = decrypted_font->GetSize();
        uint64_t offset = 0;
        std::vector<u8> chunk_buf(std::min<uint64_t>(k_chunk, std::max<uint64_t>(total_size, 1)));
        while (offset < total_size)
        {
            const uint64_t n = std::min<uint64_t>(chunk_buf.size(), total_size - offset);
            if (decrypted_font->ReadBytes(chunk_buf.data(), n, offset) != n)
            {
                LOG_ERROR(Service_AM, "Read failed while writing {}", DECRYPTED_SHARED_FONTS[i]);
                break;
            }
            if (out_file->WriteBytes(chunk_buf.data(), n, offset) != n)
            {
                LOG_ERROR(Service_AM, "Write failed while writing {}", DECRYPTED_SHARED_FONTS[i]);
                break;
            }
            offset += n;
        }
    }
}

} // namespace

WebBrowser::WebBrowser(Core::System & system_, std::shared_ptr<Applet> applet_, LibraryAppletMode applet_mode_, IWebBrowserApplet & frontend_) :
    FrontendApplet{system_, applet_, applet_mode_},
    frontend(frontend_)
{
}

WebBrowser::~WebBrowser() = default;

void WebBrowser::Initialize()
{
    FrontendApplet::Initialize();

    LOG_INFO(Service_AM, "Initializing Web Browser Applet.");
    LOG_DEBUG(Service_AM, "Initializing Applet with common_args: arg_version={}, lib_version={}, play_startup_sound={}, size={}, system_tick={}, theme_color={}", common_args.arguments_version, common_args.library_version, common_args.play_startup_sound, common_args.size, common_args.system_tick, common_args.theme_color);

    web_applet_version = WebAppletVersion{common_args.library_version};

    const auto web_arg_storage = PopInData();
    ASSERT(web_arg_storage != nullptr);

    const auto & web_arg = web_arg_storage->GetData();
    ASSERT_OR_EXECUTE(web_arg.size() >= sizeof(WebArgHeader), { return; });

    web_arg_input_tlv_map = ReadWebArgs(web_arg, web_arg_header);

    LOG_DEBUG(Service_AM, "WebArgHeader: total_tlv_entries={}, shim_kind={}", web_arg_header.total_tlv_entries, web_arg_header.shim_kind);

    ExtractSharedFonts(system);

    switch (web_arg_header.shim_kind)
    {
    case ShimKind::Shop:
        InitializeShop();
        break;
    case ShimKind::Login:
        InitializeLogin();
        break;
    case ShimKind::Offline:
        InitializeOffline();
        break;
    case ShimKind::Share:
        InitializeShare();
        break;
    case ShimKind::Web:
        InitializeWeb();
        break;
    case ShimKind::Wifi:
        InitializeWifi();
        break;
    case ShimKind::Lobby:
        InitializeLobby();
        break;
    default:
        ASSERT_MSG(false, "Invalid ShimKind={}", web_arg_header.shim_kind);
        break;
    }
}

Result WebBrowser::GetStatus() const
{
    return status;
}

void WebBrowser::ExecuteInteractive()
{
    UNIMPLEMENTED_MSG("WebSession is not implemented");
}

void WebBrowser::Execute()
{
    switch (web_arg_header.shim_kind)
    {
    case ShimKind::Shop:
        ExecuteShop();
        break;
    case ShimKind::Login:
        ExecuteLogin();
        break;
    case ShimKind::Offline:
        ExecuteOffline();
        break;
    case ShimKind::Share:
        ExecuteShare();
        break;
    case ShimKind::Web:
        ExecuteWeb();
        break;
    case ShimKind::Wifi:
        ExecuteWifi();
        break;
    case ShimKind::Lobby:
        ExecuteLobby();
        break;
    default:
        ASSERT_MSG(false, "Invalid ShimKind={}", web_arg_header.shim_kind);
        WebBrowserExit(WebExitReason::EndButtonPressed);
        break;
    }
}

void WebBrowser::ExtractOfflineRomFS()
{
    LOG_DEBUG(Service_AM, "Extracting RomFS to {}", Common::FS::PathToUTF8String(offline_cache_dir));

    if (!offline_romfs)
    {
        LOG_ERROR(Service_AM, "ExtractOfflineRomFS: no offline RomFS file");
        return;
    }

    IVirtualDirectoryPtr extracted_romfs(offline_romfs->ExtractRomFS());
    if (!extracted_romfs)
    {
        LOG_ERROR(Service_AM, "ExtractOfflineRomFS: RomFS extract failed");
        return;
    }

    ISystemloader & loader = system.GetSystemloader();
    const std::string cache_path_utf8 = Common::FS::PathToUTF8String(offline_cache_dir);
    IVirtualDirectoryPtr temp_dir(loader.Filesystem().CreateDirectory(cache_path_utf8.c_str(), VirtualFileOpenMode::ReadWrite));
    if (!temp_dir)
    {
        LOG_ERROR(Service_AM, "ExtractOfflineRomFS: could not create cache directory {}", cache_path_utf8);
        return;
    }

    if (!CopyVirtualDirectoryTree(extracted_romfs, temp_dir))
    {
        LOG_ERROR(Service_AM, "ExtractOfflineRomFS: failed to copy RomFS into {}", cache_path_utf8);
    }
}

void WebBrowser::WebBrowserExit(WebExitReason exit_reason, std::string last_url)
{
    if ((web_arg_header.shim_kind == ShimKind::Share && web_applet_version >= WebAppletVersion::Version196608) || (web_arg_header.shim_kind == ShimKind::Web && web_applet_version >= WebAppletVersion::Version524288))
    {
        // TODO: Push Output TLVs instead of a WebCommonReturnValue
    }

    WebCommonReturnValue web_common_return_value;

    web_common_return_value.exit_reason = exit_reason;
    std::memcpy(&web_common_return_value.last_url, last_url.data(), last_url.size());
    web_common_return_value.last_url_size = last_url.size();

    LOG_DEBUG(Service_AM, "WebCommonReturnValue: exit_reason={}, last_url={}, last_url_size={}", exit_reason, last_url, last_url.size());

    complete = true;
    std::vector<u8> out_data(sizeof(WebCommonReturnValue));
    std::memcpy(out_data.data(), &web_common_return_value, out_data.size());
    PushOutData(std::make_shared<IStorage>(system, std::move(out_data)));
    Exit();
}

Result WebBrowser::RequestExit()
{
    frontend.Close();
    R_SUCCEED();
}

bool WebBrowser::InputTLVExistsInMap(WebArgInputTLVType input_tlv_type) const
{
    return web_arg_input_tlv_map.find(input_tlv_type) != web_arg_input_tlv_map.end();
}

void WebBrowser::ExtractRom(void * this_)
{
    if (this_ != nullptr)
    {
        ((WebBrowser *)this_)->ExtractOfflineRomFS();
    }
}

void WebBrowser::OpenWebPage(void * this_, uint32_t exit_reason_raw, const char * last_url_utf8)
{
    if (this_ != nullptr)
    {
        ((WebBrowser *)this_)->WebBrowserExit((WebExitReason)exit_reason_raw, last_url_utf8 != nullptr ? std::string(last_url_utf8) : std::string{});
    }
}

std::optional<std::vector<u8>> WebBrowser::GetInputTLVData(WebArgInputTLVType input_tlv_type)
{
    const auto map_it = web_arg_input_tlv_map.find(input_tlv_type);

    if (map_it == web_arg_input_tlv_map.end())
    {
        return std::nullopt;
    }

    return map_it->second;
}

void WebBrowser::InitializeShop()
{
}

void WebBrowser::InitializeLogin()
{
}

void WebBrowser::InitializeOffline()
{
    const auto document_path = ParseStringValue(GetInputTLVData(WebArgInputTLVType::DocumentPath).value());
    const auto document_kind = ParseRawValue<DocumentKind>(GetInputTLVData(WebArgInputTLVType::DocumentKind).value());

    std::string additional_paths;

    switch (document_kind)
    {
    case DocumentKind::OfflineHtmlPage:
    default:
        title_id = system.GetApplicationProcessProgramID();
        nca_type = LoaderContentRecordType::HtmlDocument;
        additional_paths = "html-document";
        break;
    case DocumentKind::ApplicationLegalInformation:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::ApplicationID).value());
        nca_type = LoaderContentRecordType::LegalInformation;
        break;
    case DocumentKind::SystemDataPage:
        title_id = ParseRawValue<u64>(GetInputTLVData(WebArgInputTLVType::SystemDataID).value());
        nca_type = LoaderContentRecordType::Data;
        break;
    }

    static constexpr std::array<const char *, 3> RESOURCE_TYPES{
        "manual",
        "legal_information",
        "system_data",
    };

    offline_cache_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / fmt::format("offline_web_applet_{}/{:016X}", RESOURCE_TYPES[static_cast<u32>(document_kind) - 1], title_id);
    offline_document = Common::FS::ConcatPathSafe(offline_cache_dir, fmt::format("{}/{}", additional_paths, document_path));
}

void WebBrowser::InitializeShare()
{
}

void WebBrowser::InitializeWeb()
{
    external_url = ParseStringValue(GetInputTLVData(WebArgInputTLVType::InitialURL).value());

    // Resolve Nintendo CDN URLs.
    external_url = ResolveURL(external_url);
}

void WebBrowser::InitializeWifi()
{
}

void WebBrowser::InitializeLobby()
{
}

void WebBrowser::ExecuteShop()
{
    LOG_WARNING(Service_AM, "(STUBBED) called, Shop Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLogin()
{
    LOG_WARNING(Service_AM, "(STUBBED) called, Login Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteOffline()
{
    // TODO (Morph): This is a hack for WebSession foreground web applets such as those used by
    //               Super Mario 3D All-Stars.
    // TODO (Morph): Implement WebSession.
    if (applet_mode == LibraryAppletMode::AllForegroundInitiallyHidden) 
    {
        LOG_WARNING(Service_AM, "WebSession is not implemented");
        return;
    }

    const auto main_url = GetMainURL(Common::FS::PathToUTF8String(offline_document));

    if (!Common::FS::Exists(main_url)) 
    {
        offline_romfs = GetOfflineRomFS(system, title_id, nca_type);

        if (!offline_romfs)
        {
            LOG_ERROR(Service_AM, "RomFS with title_id={:016X} and nca_type={:02X} cannot be extracted!", title_id, static_cast<unsigned>(nca_type));
            WebBrowserExit(WebExitReason::WindowClosed);
            return;
        }
    }

    LOG_INFO(Service_AM, "Opening offline document at {}", Common::FS::PathToUTF8String(offline_document));

    const std::string local_path_utf8 = Common::FS::PathToUTF8String(offline_document);
    frontend.OpenLocalWebPage(local_path_utf8.c_str(), this, ExtractRom, this, OpenWebPage);
}

void WebBrowser::ExecuteShare()
{
    LOG_WARNING(Service_AM, "(STUBBED) called, Share Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteWeb()
{
    LOG_INFO(Service_AM, "Opening external URL at {}", external_url);
    UNIMPLEMENTED();
}

void WebBrowser::ExecuteWifi()
{
    LOG_WARNING(Service_AM, "(STUBBED) called, Wifi Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

void WebBrowser::ExecuteLobby()
{
    LOG_WARNING(Service_AM, "(STUBBED) called, Lobby Applet is not implemented");
    WebBrowserExit(WebExitReason::EndButtonPressed);
}

} // namespace Service::AM::Frontend