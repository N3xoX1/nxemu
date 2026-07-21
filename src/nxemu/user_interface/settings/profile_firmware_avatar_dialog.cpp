#include "profile_firmware_avatar_dialog.h"
#include "user_interface/html_utils.h"
#include "user_interface/notification.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/stb.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/swap.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/system_loader.h>
#include <sciter_element.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace
{
enum
{
    TIMER_WAIT_LOAD = 1,
    TIMER_LOAD_IMAGES = 2,
    IMAGE_BATCH_SIZE = 16,
};

std::mutex g_uriCacheMutex;
std::vector<std::string> g_uriCache;

const std::string & CachedDataUri(uint32_t index, const std::string & path)
{
    std::lock_guard lock(g_uriCacheMutex);
    if (index >= g_uriCache.size())
    {
        g_uriCache.resize(index + 1);
    }
    if (g_uriCache[index].empty())
    {
        g_uriCache[index] = ImageDataUriFromFile(path.c_str());
    }
    return g_uriCache[index];
}

int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F')
    {
        return 10 + (c - 'A');
    }
    return -1;
}

constexpr uint64_t AvatarImageTitleId = 0x010000000000080AULL;
constexpr int ProfileDimension = 256;
constexpr int ThumbDimension = 72;
constexpr std::size_t ProfilePixelCount = (std::size_t)ProfileDimension * ProfileDimension;
constexpr std::size_t ProfileRgbaSize = ProfilePixelCount * 4;

std::filesystem::path AvatarCacheDir()
{
    return Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / "firmware_avatars";
}

std::string AvatarCacheFileName(uint32_t index, int dim)
{
    return std::to_string(index) + "_t." + std::to_string(dim) + ".png";
}

uint32_t ReadBe32(const uint8_t * data)
{
    u32_be value;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

std::vector<uint8_t> DecompressYaz0(const std::vector<uint8_t> & input)
{
    if (input.size() < 16 || std::memcmp(input.data(), "Yaz0", 4) != 0)
    {
        return {};
    }

    const uint32_t decoded_length = ReadBe32(input.data() + 4);
    if (decoded_length == 0 || decoded_length > (16 * 1024 * 1024))
    {
        return {};
    }

    std::size_t input_offset = 16;
    std::vector<uint8_t> output(decoded_length);
    std::size_t output_offset = 0;

    uint16_t mask = 0;
    uint8_t header = 0;

    while (output_offset < decoded_length)
    {
        mask >>= 1;
        if (mask == 0)
        {
            if (input_offset >= input.size())
            {
                break;
            }
            header = input[input_offset++];
            mask = 0x80;
        }

        if ((header & mask) != 0)
        {
            if (input_offset >= input.size() || output_offset >= output.size())
            {
                break;
            }
            output[output_offset++] = input[input_offset++];
        }
        else
        {
            if (input_offset + 1 >= input.size())
            {
                break;
            }

            const uint8_t byte1 = input[input_offset++];
            const uint8_t byte2 = input[input_offset++];
            const uint32_t dist = ((uint32_t)(byte1 & 0xF) << 8) | byte2;
            std::size_t position = output_offset - (dist + 1);

            uint32_t length = (uint32_t)byte1 >> 4;
            if (length == 0)
            {
                if (input_offset >= input.size())
                {
                    break;
                }
                length = (uint32_t)input[input_offset++] + 0x12;
            }
            else
            {
                length += 2;
            }

            if (position >= output.size())
            {
                break;
            }

            for (uint32_t i = 0; i < length && output_offset < decoded_length; ++i)
            {
                output[output_offset++] = output[position++];
            }
        }
    }

    if (output_offset != decoded_length)
    {
        return {};
    }
    return output;
}

bool PathLooksLikeAvatarSzs(const std::string & path)
{
    // Firmware stores avatars as chara/00000001.szs (directory name + numbered szs).
    // Match the full path so "chara" in a parent directory is recognized.
    const std::string lower = Common::ToLower(path);
    return lower.find("chara") != std::string::npos && lower.find("szs") != std::string::npos;
}

std::vector<uint8_t> UnpremultiplyToStraightRgba(const std::vector<uint8_t> & premul)
{
    std::vector<uint8_t> straight = premul;
    for (std::size_t i = 0; i + 3 < straight.size(); i += 4)
    {
        const uint8_t a = straight[i + 3];
        if (a == 0)
        {
            straight[i] = 0;
            straight[i + 1] = 0;
            straight[i + 2] = 0;
            continue;
        }
        if (a == 255)
        {
            continue;
        }
        straight[i] = (uint8_t)(std::min(255, (straight[i] * 255) / a));
        straight[i + 1] = (uint8_t)(std::min(255, (straight[i + 1] * 255) / a));
        straight[i + 2] = (uint8_t)(std::min(255, (straight[i + 2] * 255) / a));
    }
    return straight;
}

std::vector<uint8_t> DownsamplePremulRgba(const std::vector<uint8_t> & premul_rgba, int src_dim, int dst_dim)
{
    std::vector<uint8_t> out((std::size_t)(dst_dim) * (std::size_t)(dst_dim) * 4);
    for (int y = 0; y < dst_dim; ++y)
    {
        const int sy = y * src_dim / dst_dim;
        for (int x = 0; x < dst_dim; ++x)
        {
            const int sx = x * src_dim / dst_dim;
            const uint8_t * src = &premul_rgba[((std::size_t)(sy) * src_dim + sx) * 4];
            uint8_t * dst = &out[((std::size_t)(y) * dst_dim + x) * 4];
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = src[3];
        }
    }
    return out;
}

bool WriteStraightRgbaPng(const std::vector<uint8_t> & straight_rgba, int dim, const std::filesystem::path & path)
{
    if (!Common::FS::CreateParentDirs(path))
    {
        return false;
    }

    const std::string path_utf8 = Common::FS::PathToUTF8String(path);
    return stbi_write_png(path_utf8.c_str(), dim, dim, 4, straight_rgba.data(), dim * 4) != 0;
}

std::vector<uint8_t> LoadStraightRgba256(const std::string & path_utf8)
{
    std::ifstream file(path_utf8.c_str(), std::ios::binary);
    if (!file)
    {
        return {};
    }

    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (encoded.empty())
    {
        return {};
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char * pixels = stbi_load_from_memory(encoded.data(), (int)(encoded.size()), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr)
    {
        return {};
    }

    std::vector<uint8_t> rgba;
    if (width == ProfileDimension && height == ProfileDimension)
    {
        rgba.assign(pixels, pixels + ProfileRgbaSize);
    }
    stbi_image_free(pixels);
    return rgba;
}

bool CacheFileReady(const std::filesystem::path & path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0 && !ec;
}

void CollectAvatarFiles(IVirtualDirectory * directory, const std::string & path_prefix,
                        std::vector<std::pair<std::string, std::vector<uint8_t>>> & out_files)
{
    if (directory == nullptr)
    {
        return;
    }

    {
        IVirtualFileListPtr files(directory->GetFiles());
        if (files)
        {
            const uint32_t count = files->GetSize();
            for (uint32_t i = 0; i < count; ++i)
            {
                IVirtualFilePtr file(files->GetItem(i));
                if (!file)
                {
                    continue;
                }

                const char * name_cstr = file->GetName();
                if (name_cstr == nullptr)
                {
                    continue;
                }

                const std::string name(name_cstr);
                const std::string full_path = path_prefix + name;
                if (!PathLooksLikeAvatarSzs(full_path))
                {
                    continue;
                }

                std::vector<uint8_t> data = file.ReadAllBytes();
                if (!data.empty())
                {
                    out_files.emplace_back(full_path, std::move(data));
                }
            }
        }
    }

    {
        IVirtualDirectoryListPtr subdirs(directory->GetSubdirectories());
        if (!subdirs)
        {
            return;
        }

        const uint32_t count = subdirs->GetSize();
        for (uint32_t i = 0; i < count; ++i)
        {
            IVirtualDirectoryPtr subdir(subdirs->GetItem(i));
            if (!subdir)
            {
                continue;
            }
            const char * name_cstr = subdir->GetName();
            const std::string name = name_cstr != nullptr ? name_cstr : "";
            CollectAvatarFiles(subdir.Get(), path_prefix + name + "/", out_files);
        }
    }
}

std::vector<FirmwareProfileAvatar> EnumerateCachedAvatars()
{
    std::vector<FirmwareProfileAvatar> avatars;
    const std::filesystem::path cache_dir = AvatarCacheDir();
    for (uint32_t index = 0;; ++index)
    {
        const std::filesystem::path full = cache_dir / AvatarCacheFileName(index, ProfileDimension);
        const std::filesystem::path thumb = cache_dir / AvatarCacheFileName(index, ThumbDimension);
        if (!CacheFileReady(full) || !CacheFileReady(thumb))
        {
            break;
        }

        FirmwareProfileAvatar avatar;
        avatar.full_path = Common::FS::PathToUTF8String(full);
        avatar.thumb_path = Common::FS::PathToUTF8String(thumb);
        avatars.push_back(std::move(avatar));
    }
    return avatars;
}

void BuildAvatarCacheFromFirmware(ISystemModules & modules, FirmwareAvatarLoadState & state)
{
    ISystemloader & loader = modules.Systemloader();
    FileSysNCAPtr nca(loader.GetContentProviderEntry(AvatarImageTitleId, LoaderContentRecordType::Data));
    if (!nca || nca->GetStatus() != LoaderResultStatus::Success)
    {
        LOG_WARNING(Service_ACC, "AvatarImage NCA 010000000000080A not found or failed to open");
        state.done.store(true);
        return;
    }

    IVirtualFilePtr romfs_file(nca->GetRomFS());
    if (!romfs_file)
    {
        LOG_WARNING(Service_ACC, "AvatarImage NCA has no RomFS section");
        state.done.store(true);
        return;
    }

    IVirtualDirectoryPtr romfs(romfs_file->ExtractRomFS());
    if (!romfs)
    {
        LOG_WARNING(Service_ACC, "AvatarImage RomFS extract failed");
        state.done.store(true);
        return;
    }

    std::vector<std::pair<std::string, std::vector<uint8_t>>> compressed_files;
    CollectAvatarFiles(romfs.Get(), "", compressed_files);
    if (compressed_files.empty())
    {
        LOG_WARNING(Service_ACC, "AvatarImage RomFS contained no chara*.szs avatars");
        state.done.store(true);
        return;
    }

    std::sort(compressed_files.begin(), compressed_files.end(),
              [](const auto & a, const auto & b) { return a.first < b.first; });

    const std::filesystem::path cache_dir = AvatarCacheDir();
    void(Common::FS::CreateDir(cache_dir));

    uint32_t index = 0;
    for (auto & [name, compressed] : compressed_files)
    {
        const std::vector<uint8_t> rgba = DecompressYaz0(compressed);
        if (rgba.size() != ProfileRgbaSize)
        {
            LOG_WARNING(Service_ACC, "Skipping firmware avatar '{}' (unexpected size {})", name, rgba.size());
            continue;
        }

        const std::vector<uint8_t> straight = UnpremultiplyToStraightRgba(rgba);
        const std::vector<uint8_t> thumb = DownsamplePremulRgba(straight, ProfileDimension, ThumbDimension);

        const std::filesystem::path full_path = cache_dir / AvatarCacheFileName(index, ProfileDimension);
        const std::filesystem::path thumb_path = cache_dir / AvatarCacheFileName(index, ThumbDimension);
        if (!WriteStraightRgbaPng(straight, ProfileDimension, full_path) ||
            !WriteStraightRgbaPng(thumb, ThumbDimension, thumb_path))
        {
            LOG_WARNING(Service_ACC, "Failed to write firmware avatar cache for '{}'", name);
            continue;
        }

        FirmwareProfileAvatar avatar;
        avatar.full_path = Common::FS::PathToUTF8String(full_path);
        avatar.thumb_path = Common::FS::PathToUTF8String(thumb_path);
        {
            std::lock_guard lock(state.mutex);
            state.avatars.push_back(std::move(avatar));
        }
        ++index;
    }

    state.done.store(true);
}

void PngToMemory(void * context, void * data, int len)
{
    auto * png = (std::vector<uint8_t> *)context;
    const auto * bytes = (unsigned char *)data;
    png->insert(png->end(), bytes, bytes + len);
}

std::vector<uint8_t> BuildFirmwareAvatarProfileImage(const std::vector<uint8_t> & straight, uint8_t background_r, uint8_t background_g, uint8_t background_b)
{
    if (straight.size() != ProfileRgbaSize)
    {
        return {};
    }

    std::vector<uint8_t> rgb(ProfilePixelCount * 3);
    for (std::size_t i = 0; i < ProfilePixelCount; ++i)
    {
        rgb[i * 3 + 0] = background_r;
        rgb[i * 3 + 1] = background_g;
        rgb[i * 3 + 2] = background_b;
    }

    for (std::size_t i = 0; i < ProfilePixelCount; ++i)
    {
        const uint8_t * px = &straight[i * 4];
        const uint32_t a = px[3];
        if (a == 0)
        {
            continue;
        }
        if (a == 255)
        {
            rgb[i * 3 + 0] = px[0];
            rgb[i * 3 + 1] = px[1];
            rgb[i * 3 + 2] = px[2];
            continue;
        }

        const uint32_t inv_a = 255 - a;
        rgb[i * 3 + 0] = (uint8_t)((px[0] * a + background_r * inv_a) / 255);
        rgb[i * 3 + 1] = (uint8_t)((px[1] * a + background_g * inv_a) / 255);
        rgb[i * 3 + 2] = (uint8_t)((px[2] * a + background_b * inv_a) / 255);
    }

    std::vector<uint8_t> png;
    if (stbi_write_png_to_func(PngToMemory, &png, ProfileDimension, ProfileDimension, 3, rgb.data(), ProfileDimension * 3) == 0)
    {
        return {};
    }
    return png;
}
}

ProfileFirmwareAvatarDialog::ProfileFirmwareAvatarDialog(ISciterUI & sciterUI, SystemModules & modules) :
    m_sciterUI(sciterUI),
    m_modules(modules),
    m_window(nullptr),
    m_selectedIndex(-1),
    m_backgroundColor("#FFFFFF"),
    m_selection{},
    m_changed(false),
    m_nextImageIndex(0)
{
}

ProfileFirmwareAvatarDialog::~ProfileFirmwareAvatarDialog() = default;

bool ProfileFirmwareAvatarDialog::Display(void * parentWindow, PendingProfileImage & outSelection)
{
    enum
    {
        WINDOW_WIDTH = 560,
    };

    m_selectedIndex = -1;
    m_backgroundColor = "#FFFFFF";
    m_selection = {};
    m_changed = false;
    m_window = nullptr;
    m_avatars.clear();
    m_pngPaths.clear();
    m_nextImageIndex = 0;
    m_loadState.reset();
    outSelection = {};

    if (!m_sciterUI.WindowCreate(parentWindow, "profile_firmware_avatar_dialog.html", 0, 0, WINDOW_WIDTH, 0, SUIW_CHILD, m_window))
    {
        return false;
    }
    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement empty(root.GetElementByID("FirmwareAvatarEmpty"));
        SciterElement grid(root.GetElementByID("FirmwareAvatarGrid"));
        if (empty.IsValid())
        {
            empty.SetText("Loading firmware avatars...");
            empty.SetStyleAttribute("display", "block");
        }
        if (grid.IsValid())
        {
            grid.SetStyleAttribute("display", "none");
        }

        AttachClickHandler(m_sciterUI, root.GetElementByID("firmwareAvatarChoose"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("FirmwareAvatarColors"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("FirmwareAvatarGrid"), this);

        m_sciterUI.AttachHandler(root, IID_ITIMERSINK, (ITimerSink *)this);

        m_avatars = EnumerateCachedAvatars();
        if (!m_avatars.empty())
        {
            PopulateGridStructure();
            if (!m_pngPaths.empty())
            {
                root.SetTimer(1, (uint32_t *)TIMER_LOAD_IMAGES);
            }
        }
        else
        {
            StartAvatarLoad();
            root.SetTimer(16, (uint32_t *)TIMER_WAIT_LOAD);
        }
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
    m_window->RunModal();

    if (m_changed)
    {
        outSelection = m_selection;
    }
    return m_changed;
}

void ProfileFirmwareAvatarDialog::StartAvatarLoad()
{
    m_loadState = std::make_shared<FirmwareAvatarLoadState>();
    std::shared_ptr<FirmwareAvatarLoadState> state = m_loadState;
    SystemModules * modules = &m_modules;
    std::thread([state, modules]() {
        if (modules != nullptr && modules->IsValid())
        {
            BuildAvatarCacheFromFirmware(modules->Modules(), *state);
        }
        else
        {
            state->done.store(true);
        }
    }).detach();
}

bool ProfileFirmwareAvatarDialog::OnTimer(SCITER_ELEMENT /*element*/, uint32_t * timerId)
{
    if (timerId == (uint32_t *)TIMER_WAIT_LOAD)
    {
        if (m_loadState == nullptr)
        {
            return false;
        }

        SyncAvatarsFromLoad();
        if (m_nextImageIndex < m_pngPaths.size())
        {
            ApplyNextImageBatch();
        }

        if (m_loadState->done.load())
        {
            SyncAvatarsFromLoad();
            if (m_avatars.empty())
            {
                ShowEmptyAvatarsMessage();
            }
            m_loadState.reset();
            if (m_window != nullptr && m_nextImageIndex < m_pngPaths.size())
            {
                SciterElement(m_window->GetRootElement()).SetTimer(1, (uint32_t *)TIMER_LOAD_IMAGES);
            }
            return false;
        }

        return true;
    }

    if (timerId == (uint32_t *)TIMER_LOAD_IMAGES)
    {
        ApplyNextImageBatch();
        return m_nextImageIndex < m_pngPaths.size();
    }

    return false;
}

bool ProfileFirmwareAvatarDialog::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    SciterElement sourceElem(source);
    const std::string elementID = clickElem.GetAttributeByName("id");

    if (elementID == "firmwareAvatarChoose")
    {
        if (ChooseSelected())
        {
            m_changed = true;
            Close();
        }
        return true;
    }

    SciterElement walk = sourceElem.IsValid() ? sourceElem : clickElem;
    while (walk.IsValid())
    {
        const std::string walkRole = walk.GetAttribute("role");
        if (walkRole == "firmware-color")
        {
            const std::string color = walk.GetAttribute("data-color");
            if (!color.empty())
            {
                SelectBackgroundColor(color);
            }
            return true;
        }

        if (walkRole == "firmware-avatar")
        {
            const std::string indexAttr = walk.GetAttribute("data-index");
            if (!indexAttr.empty())
            {
                const int32_t newIndex = std::stoi(indexAttr);
                if (newIndex != m_selectedIndex)
                {
                    m_selectedIndex = newIndex;
                    UpdateSelectionHighlight();
                }
            }
            return true;
        }

        if (walk.GetAttributeByName("id") == "FirmwareAvatarGrid" ||
            walk.GetAttributeByName("id") == "FirmwareAvatarColors")
        {
            break;
        }
        walk = walk.GetParent();
    }

    return true;
}

void ProfileFirmwareAvatarDialog::Close()
{
    if (m_window == nullptr || m_window->IsClosed())
    {
        return;
    }
    m_window->Destroy();
}

void ProfileFirmwareAvatarDialog::PopulateGridStructure()
{
    if (m_window == nullptr || !m_modules.IsValid())
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    SciterElement grid(root.GetElementByID("FirmwareAvatarGrid"));
    SciterElement empty(root.GetElementByID("FirmwareAvatarEmpty"));
    if (!grid.IsValid())
    {
        return;
    }

    const uint32_t count = (uint32_t)(m_avatars.size());

    if (empty.IsValid())
    {
        if (count == 0)
        {
            ShowEmptyAvatarsMessage();
        }
        else
        {
            empty.SetStyleAttribute("display", "none");
        }
    }
    grid.SetStyleAttribute("display", count == 0 ? "none" : "block");

    m_pngPaths.clear();
    m_nextImageIndex = 0;

    const std::vector<FirmwareProfileAvatar> & avatars = m_avatars;
    {
        std::lock_guard lock(g_uriCacheMutex);
        if (g_uriCache.size() != count)
        {
            g_uriCache.clear();
        }
    }

    std::string html;
    m_pngPaths.reserve(count);
    for (uint32_t i = 0; i < count && i < avatars.size(); ++i)
    {
        if (avatars[i].thumb_path.empty())
        {
            continue;
        }

        m_pngPaths.emplace_back(avatars[i].thumb_path);
        html += stdstr_f(
            "<div class=\"firmware-avatar-tile%s\" role=\"firmware-avatar\" data-index=\"%u\">"
            "<img />"
            "</div>",
            static_cast<int32_t>(i) == m_selectedIndex ? " selected" : "",
            i);
    }

    grid.SetHTML(reinterpret_cast<const uint8_t *>(html.c_str()), html.size());
    m_sciterUI.AttachHandler(grid, IID_ICLICKSINK, (IClickSink *)this);
    ApplyBackgroundColorPreview();
}

void ProfileFirmwareAvatarDialog::SyncAvatarsFromLoad()
{
    if (m_loadState == nullptr || m_window == nullptr)
    {
        return;
    }

    std::vector<FirmwareProfileAvatar> pending;
    {
        std::lock_guard lock(m_loadState->mutex);
        if (m_loadState->avatars.size() <= m_avatars.size())
        {
            return;
        }
        pending.assign(m_loadState->avatars.begin() + m_avatars.size(), m_loadState->avatars.end());
    }

    SciterElement root(m_window->GetRootElement());
    SciterElement grid(root.GetElementByID("FirmwareAvatarGrid"));
    SciterElement empty(root.GetElementByID("FirmwareAvatarEmpty"));
    if (!grid.IsValid())
    {
        return;
    }

    const uint32_t start_index = (uint32_t)(m_avatars.size());
    m_avatars.insert(m_avatars.end(), pending.begin(), pending.end());

    if (empty.IsValid())
    {
        empty.SetStyleAttribute("display", "none");
    }
    grid.SetStyleAttribute("display", "block");

    std::string html;
    html.reserve(pending.size() * 128);
    for (uint32_t i = start_index; i < (uint32_t)(m_avatars.size()); ++i)
    {
        if (m_avatars[i].thumb_path.empty())
        {
            continue;
        }

        m_pngPaths.emplace_back(m_avatars[i].thumb_path);
        html += stdstr_f(
            "<div class=\"firmware-avatar-tile%s\" role=\"firmware-avatar\" data-index=\"%u\">"
            "<img />"
            "</div>",
            (int32_t)(i) == m_selectedIndex ? " selected" : "",
            i);
    }

    if (!html.empty())
    {
        grid.SetHTML(reinterpret_cast<const uint8_t *>(html.c_str()), html.size(),
                     SciterElement::SIH_APPEND_AFTER_LAST);
        m_sciterUI.AttachHandler(grid, IID_ICLICKSINK, (IClickSink *)this);
        ApplyBackgroundColorPreview();
    }
}

void ProfileFirmwareAvatarDialog::ShowEmptyAvatarsMessage()
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    SciterElement grid(root.GetElementByID("FirmwareAvatarGrid"));
    SciterElement empty(root.GetElementByID("FirmwareAvatarEmpty"));
    if (empty.IsValid())
    {
        empty.SetText("No firmware avatars were found. Ensure a complete system firmware install that includes AvatarImage (010000000000080A).");
        empty.SetStyleAttribute("display", "block");
    }
    if (grid.IsValid())
    {
        grid.SetStyleAttribute("display", "none");
    }
}

void ProfileFirmwareAvatarDialog::ApplyNextImageBatch()
{
    if (m_window == nullptr)
    {
        m_nextImageIndex = static_cast<uint32_t>(m_pngPaths.size());
        return;
    }

    SciterElement grid(SciterElement(m_window->GetRootElement()).GetElementByID("FirmwareAvatarGrid"));
    if (!grid.IsValid())
    {
        m_nextImageIndex = static_cast<uint32_t>(m_pngPaths.size());
        return;
    }

    const uint32_t tileCount = (std::min)(grid.GetChildCount(), static_cast<uint32_t>(m_pngPaths.size()));
    const uint32_t end = (std::min)(m_nextImageIndex + IMAGE_BATCH_SIZE, tileCount);
    for (; m_nextImageIndex < end; ++m_nextImageIndex)
    {
        SciterElement tile = grid.GetChild(m_nextImageIndex);
        if (!tile.IsValid())
        {
            continue;
        }

        SciterElement img = tile.FindFirst("img");
        if (!img.IsValid())
        {
            continue;
        }

        const std::string & dataUri = CachedDataUri(m_nextImageIndex, m_pngPaths[m_nextImageIndex]);
        if (!dataUri.empty())
        {
            img.SetAttribute("src", dataUri.c_str());
        }
    }
}

void ProfileFirmwareAvatarDialog::UpdateSelectionHighlight()
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement grid(SciterElement(m_window->GetRootElement()).GetElementByID("FirmwareAvatarGrid"));
    if (!grid.IsValid())
    {
        return;
    }

    for (uint32_t i = 0; i < grid.GetChildCount(); ++i)
    {
        SciterElement tile = grid.GetChild(i);
        if (!tile.IsValid())
        {
            continue;
        }

        const std::string indexAttr = tile.GetAttribute("data-index");
        if (indexAttr.empty())
        {
            continue;
        }

        if (std::stoi(indexAttr) == m_selectedIndex)
        {
            tile.AddClassName("selected");
        }
        else
        {
            tile.RemoveClassName("selected");
        }
    }
}

void ProfileFirmwareAvatarDialog::ApplyBackgroundColorPreview()
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement grid(SciterElement(m_window->GetRootElement()).GetElementByID("FirmwareAvatarGrid"));
    if (!grid.IsValid())
    {
        return;
    }

    for (uint32_t i = 0; i < grid.GetChildCount(); ++i)
    {
        SciterElement tile = grid.GetChild(i);
        if (tile.IsValid())
        {
            tile.SetStyleAttribute("background-color", m_backgroundColor.c_str());
        }
    }
}

void ProfileFirmwareAvatarDialog::SelectBackgroundColor(const std::string & colorCss)
{
    m_backgroundColor = colorCss;
    ApplyBackgroundColorPreview();

    if (m_window == nullptr)
    {
        return;
    }

    SciterElement colors(SciterElement(m_window->GetRootElement()).GetElementByID("FirmwareAvatarColors"));
    if (!colors.IsValid())
    {
        return;
    }

    for (uint32_t i = 0; i < colors.GetChildCount(); ++i)
    {
        SciterElement swatch = colors.GetChild(i);
        if (!swatch.IsValid())
        {
            continue;
        }

        if (swatch.GetAttribute("data-color") == m_backgroundColor)
        {
            swatch.AddClassName("selected");
        }
        else
        {
            swatch.RemoveClassName("selected");
        }
    }
}

bool ProfileFirmwareAvatarDialog::ParseSelectedColor(uint8_t & r, uint8_t & g, uint8_t & b) const
{
    r = 255;
    g = 255;
    b = 255;

    if (m_backgroundColor.size() != 7 || m_backgroundColor[0] != '#')
    {
        return false;
    }

    const int rh = HexDigit(m_backgroundColor[1]);
    const int rl = HexDigit(m_backgroundColor[2]);
    const int gh = HexDigit(m_backgroundColor[3]);
    const int gl = HexDigit(m_backgroundColor[4]);
    const int bh = HexDigit(m_backgroundColor[5]);
    const int bl = HexDigit(m_backgroundColor[6]);
    if (rh < 0 || rl < 0 || gh < 0 || gl < 0 || bh < 0 || bl < 0)
    {
        return false;
    }

    r = static_cast<uint8_t>((rh << 4) | rl);
    g = static_cast<uint8_t>((gh << 4) | gl);
    b = static_cast<uint8_t>((bh << 4) | bl);
    return true;
}

bool ProfileFirmwareAvatarDialog::ChooseSelected()
{
    if (m_selectedIndex < 0)
    {
        Notification::GetInstance().DisplayError("Select a firmware avatar first.", "Profiles");
        return false;
    }

    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    ParseSelectedColor(r, g, b);

    const std::vector<FirmwareProfileAvatar> & avatars = m_avatars;
    if (static_cast<uint32_t>(m_selectedIndex) >= avatars.size())
    {
        Notification::GetInstance().DisplayError("Select a firmware avatar first.", "Profiles");
        return false;
    }

    const std::vector<uint8_t> straight = LoadStraightRgba256(avatars[m_selectedIndex].full_path);
    std::vector<uint8_t> data = BuildFirmwareAvatarProfileImage(straight, r, g, b);
    if (data.empty())
    {
        Notification::GetInstance().DisplayError("Unable to build the selected avatar image.", "Profiles");
        return false;
    }

    m_selection = {};
    m_selection.data = std::move(data);
    return true;
}
