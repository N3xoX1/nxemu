#include "profile_image_writer.h"

#include "default_profile_image.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/stb.h"
#include <cstring>
#include <vector>
namespace
{
constexpr int ProfileDimension = 256;
constexpr std::size_t ProfilePixelCount = static_cast<std::size_t>(ProfileDimension) * ProfileDimension;

void JpgToMemory(void * context, void * data, int len)
{
    std::vector<uint8_t> * jpg = (std::vector<uint8_t> *)context;
    const unsigned char * bytes = (const unsigned char *)data;
    jpg->insert(jpg->end(), bytes, bytes + len);
}

bool WriteJpegRgb(const uint8_t * rgb, const std::filesystem::path & destination)
{
    if (!Common::FS::CreateParentDirs(destination))
    {
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(destination, ec))
    {
        if (!Common::FS::RemoveFile(destination))
        {
            return false;
        }
    }

    std::vector<uint8_t> jpeg;
    if (!stbi_write_jpg_to_func(JpgToMemory, &jpeg, ProfileDimension, ProfileDimension, STBI_rgb, rgb, 100))
    {
        return false;
    }

    Common::FS::IOFile file(destination, Common::FS::FileAccessMode::Write, Common::FS::FileType::BinaryFile);
    if (!file.IsOpen() || !file.SetSize(jpeg.size()) || file.Write(jpeg) != jpeg.size())
    {
        return false;
    }
    return true;
}
} // namespace

bool WriteProfileJpegFromMemory(const uint8_t * data, size_t size, const std::filesystem::path & destination)
{
    if (data == nullptr || size == 0)
    {
        return false;
    }

    int32_t width = 0, height = 0, channels = 0;
    unsigned char * pixels = stbi_load_from_memory(data, static_cast<int>(size), &width, &height, &channels, STBI_rgb);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (pixels != nullptr)
        {
            stbi_image_free(pixels);
        }
        return false;
    }

    std::vector<uint8_t> resized(ProfilePixelCount * 3);
    if (width != ProfileDimension || height != ProfileDimension)
    {
        if (!stbir_resize_uint8(pixels, width, height, 0, resized.data(), ProfileDimension, ProfileDimension, 0, STBI_rgb))
        {
            stbi_image_free(pixels);
            return false;
        }
        stbi_image_free(pixels);
        pixels = nullptr;
    }
    else
    {
        std::memcpy(resized.data(), pixels, resized.size());
        stbi_image_free(pixels);
        pixels = nullptr;
    }
    return WriteJpegRgb(resized.data(), destination);
}

bool WriteDefaultProfileJpeg(const std::filesystem::path & destination)
{
    return WriteProfileJpegFromMemory(default_profile_png, default_profile_png_len, destination);
}

bool EnsureDefaultProfileJpeg(const std::filesystem::path & destination)
{
    std::error_code ec;
    if (std::filesystem::exists(destination, ec) && !ec)
    {
        return true;
    }
    return WriteDefaultProfileJpeg(destination);
}
