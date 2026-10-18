#include "firmware_avatars.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nxemu-module-spec/system_loader.h>
#include <system_error>
#include <utility>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/stb.h>

namespace
{
constexpr uint64_t kAvatarTitle = 0x010000000000080AULL;
constexpr int kSide = 256;
constexpr size_t kPixels = static_cast<size_t>(kSide) * kSide;
constexpr const char * kComplete = "complete";

std::filesystem::path Directory()
{
    return Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / "console_avatars";
}

std::filesystem::path File(size_t index)
{
    return Directory() / (std::to_string(index) + ".png");
}

std::vector<uint8_t> Expanded(const std::vector<uint8_t> & input)
{
    if (input.size() < 16 || std::memcmp(input.data(), "Yaz0", 4) != 0)
    {
        return {};
    }
    const size_t size = (static_cast<size_t>(input[4]) << 24) | (static_cast<size_t>(input[5]) << 16) | (static_cast<size_t>(input[6]) << 8) | input[7];
    if (size == 0 || size > 16 * 1024 * 1024)
    {
        return {};
    }
    std::vector<uint8_t> output(size);
    size_t in = 16, out = 0;
    uint8_t header = 0, mask = 0;
    while (out < size)
    {
        mask >>= 1;
        if (mask == 0)
        {
            if (in >= input.size())
            {
                return {};
            }
            header = input[in++];
            mask = 0x80;
        }
        if ((header & mask) != 0)
        {
            if (in >= input.size())
            {
                return {};
            }
            output[out++] = input[in++];
            continue;
        }
        if (in + 1 >= input.size())
        {
            return {};
        }
        const uint8_t first = input[in++];
        const uint8_t second = input[in++];
        const size_t distance = ((static_cast<size_t>(first) & 0xF) << 8 | second) + 1;
        size_t length = first >> 4;
        if (length == 0)
        {
            if (in >= input.size())
            {
                return {};
            }
            length = static_cast<size_t>(input[in++]) + 0x12;
        }
        else
        {
            length += 2;
        }
        if (distance > out)
        {
            return {};
        }
        for (size_t i = 0; i < length && out < size; i++, out++)
        {
            output[out] = output[out - distance];
        }
    }
    return output;
}

void Collect(IVirtualDirectory & directory, const std::string & prefix, std::vector<std::pair<std::string, std::vector<uint8_t>>> & pictures)
{
    if (IVirtualFileListPtr files(directory.GetFiles()); files)
    {
        for (uint32_t index = 0; index < files->GetSize(); index++)
        {
            InterfacePtr<IVirtualFile> file(files->GetItem(index));
            const char * name = file ? file->GetName() : nullptr;
            if (name == nullptr)
            {
                continue;
            }
            std::string path = prefix + name;
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("chara") == std::string::npos || lower.find("szs") == std::string::npos)
            {
                continue;
            }
            std::vector<uint8_t> data(static_cast<size_t>(file->GetSize()));
            if (!data.empty() && file->ReadBytes(data.data(), data.size(), 0) == data.size())
            {
                pictures.emplace_back(std::move(path), std::move(data));
            }
        }
    }
    if (IVirtualDirectoryListPtr directories(directory.GetSubdirectories()); directories)
    {
        for (uint32_t index = 0; index < directories->GetSize(); index++)
        {
            IVirtualDirectoryPtr inside(directories->GetItem(index));
            if (inside)
            {
                const char * name = inside->GetName();
                Collect(*inside.Get(), prefix + (name != nullptr ? name : "") + "/", pictures);
            }
        }
    }
}

void ToMemory(void * context, void * data, int size)
{
    auto * file = static_cast<std::vector<uint8_t> *>(context);
    const auto * bytes = static_cast<const uint8_t *>(data);
    file->insert(file->end(), bytes, bytes + size);
}

bool Write(const std::filesystem::path & path, const std::vector<uint8_t> & data)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    return file.good();
}
} // namespace

FirmwareAvatars::~FirmwareAvatars()
{
    Wait();
}

void FirmwareAvatars::Load(ISystemloader & loader)
{
    if (m_loading)
    {
        return;
    }
    Wait();
    std::error_code error;
    {
        std::lock_guard lock(m_mutex);
        m_files.clear();
        if (std::filesystem::exists(Directory() / kComplete, error))
        {
            for (size_t index = 0; std::filesystem::exists(File(index), error); index++)
            {
                m_files.push_back(Common::FS::PathToUTF8String(File(index)));
            }
            return;
        }
    }
    m_loading = true;
    m_thread = std::thread([this, &loader] {
        Read(loader);
        m_loading = false;
    });
}

void FirmwareAvatars::Wait()
{
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

std::vector<std::string> FirmwareAvatars::Files()
{
    std::lock_guard lock(m_mutex);
    return m_files;
}

void FirmwareAvatars::Read(ISystemloader & loader)
{
    FileSysNCAPtr title(loader.GetContentProviderEntry(kAvatarTitle, LoaderContentRecordType::Data));
    if (!title || title->GetStatus() != LoaderResultStatus::Success)
    {
        return;
    }
    InterfacePtr<IVirtualFile> image(title->GetRomFS());
    IVirtualDirectoryPtr root(image ? image->ExtractRomFS() : nullptr);
    if (!root)
    {
        return;
    }
    std::vector<std::pair<std::string, std::vector<uint8_t>>> pictures;
    Collect(*root.Get(), "", pictures);
    std::sort(pictures.begin(), pictures.end(), [](const auto & a, const auto & b) { return a.first < b.first; });

    std::error_code error;
    std::filesystem::remove_all(Directory(), error);
    std::filesystem::create_directories(Directory(), error);
    size_t count = 0;
    for (const auto & [path, compressed] : pictures)
    {
        std::vector<uint8_t> pixels = Expanded(compressed);
        if (pixels.size() != kPixels * 4)
        {
            continue;
        }
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            const uint32_t alpha = pixels[i + 3];
            for (size_t channel = 0; channel < 3 && alpha != 0 && alpha != 255; channel++)
            {
                pixels[i + channel] = static_cast<uint8_t>(std::min<uint32_t>(255, pixels[i + channel] * 255 / alpha));
            }
        }
        std::vector<uint8_t> png;
        if (stbi_write_png_to_func(ToMemory, &png, kSide, kSide, 4, pixels.data(), kSide * 4) == 0 || !Write(File(count), png))
        {
            continue;
        }
        std::lock_guard lock(m_mutex);
        m_files.push_back(Common::FS::PathToUTF8String(File(count)));
        count++;
    }
    if (count > 0)
    {
        Write(Directory() / kComplete, {});
    }
}

void FirmwareAvatars::Forget()
{
    std::error_code error;
    std::filesystem::remove_all(Directory(), error);
}

std::vector<uint8_t> FirmwareAvatars::OnColour(const std::string & file, uint8_t red, uint8_t green, uint8_t blue)
{
    std::ifstream stream(std::filesystem::path{Common::FS::ToU8String(file)}, std::ios::binary);
    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = encoded.empty() ? nullptr : stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return {};
    }
    std::vector<uint8_t> picture;
    if (width == kSide && height == kSide)
    {
        const uint8_t colour[3] = {red, green, blue};
        std::vector<uint8_t> plain(kPixels * 3);
        for (size_t i = 0; i < kPixels; i++)
        {
            const uint32_t alpha = pixels[i * 4 + 3];
            for (size_t channel = 0; channel < 3; channel++)
            {
                plain[i * 3 + channel] = static_cast<uint8_t>((pixels[i * 4 + channel] * alpha + colour[channel] * (255 - alpha)) / 255);
            }
        }
        stbi_write_png_to_func(ToMemory, &picture, kSide, kSide, 3, plain.data(), kSide * 3);
    }
    stbi_image_free(pixels);
    return picture;
}
