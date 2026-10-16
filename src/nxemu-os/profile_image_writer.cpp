#include "profile_image_writer.h"

#include "default_profile_image.h"
#include "yuzu_common/fs/file.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/stb.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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

// The default picture of earlier versions, as the mean colour of each of its
// 8 by 8 squares: enough to know it by, whatever wrote the file.
constexpr int FormerDefaultSquares = 8;
constexpr int FormerDefaultTolerance = 12;
constexpr uint8_t FormerDefaultColours[FormerDefaultSquares * FormerDefaultSquares * 3] = {
    0, 0, 0, 4, 5, 6, 27, 28, 30, 46, 48, 50, 93, 135, 146, 100, 154, 167, 27, 32, 35, 0, 0, 0,
    68, 53, 56, 207, 112, 119, 151, 98, 109, 127, 128, 137, 92, 156, 173, 95, 140, 159, 89, 127, 140, 0, 0, 0,
    156, 88, 94, 97, 104, 117, 146, 79, 91, 113, 111, 121, 82, 139, 159, 102, 151, 173, 109, 188, 216, 4, 5, 6,
    137, 74, 79, 162, 76, 87, 201, 66, 76, 100, 107, 118, 81, 115, 133, 76, 101, 118, 68, 121, 148, 42, 49, 53,
    81, 57, 59, 160, 92, 101, 157, 98, 108, 104, 81, 93, 89, 97, 107, 74, 151, 183, 70, 160, 199, 68, 100, 116,
    26, 27, 30, 231, 73, 78, 154, 53, 65, 119, 53, 64, 106, 111, 119, 60, 131, 165, 65, 164, 210, 71, 109, 131,
    3, 3, 4, 194, 81, 88, 221, 48, 56, 148, 45, 55, 106, 110, 118, 59, 99, 123, 75, 126, 154, 24, 35, 43,
    0, 0, 0, 50, 37, 43, 131, 68, 76, 88, 47, 56, 51, 43, 48, 10, 14, 19, 1, 1, 2, 0, 0, 0,
};

bool IsFormerDefault(const unsigned char * rgb)
{
    const int side = ProfileDimension / FormerDefaultSquares;
    for (int square = 0; square < FormerDefaultSquares * FormerDefaultSquares; ++square)
    {
        const int left = (square % FormerDefaultSquares) * side;
        const int top = (square / FormerDefaultSquares) * side;
        int sums[3] = {};
        for (int y = top; y < top + side; ++y)
        {
            for (int x = left; x < left + side; ++x)
            {
                const unsigned char * pixel = rgb + (static_cast<std::size_t>(y) * ProfileDimension + x) * 3;
                sums[0] += pixel[0];
                sums[1] += pixel[1];
                sums[2] += pixel[2];
            }
        }
        for (int channel = 0; channel < 3; ++channel)
        {
            if (std::abs(sums[channel] / (side * side) - FormerDefaultColours[square * 3 + channel]) > FormerDefaultTolerance)
            {
                return false;
            }
        }
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


bool ReplaceFormerDefaultProfileJpeg(const std::filesystem::path & destination)
{
    std::ifstream file(destination, std::ios::binary);
    const std::vector<uint8_t> encoded((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    if (encoded.empty())
    {
        return false;
    }

    int32_t width = 0, height = 0, channels = 0;
    unsigned char * pixels = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &width, &height, &channels, STBI_rgb);
    const bool former = pixels != nullptr && width == ProfileDimension && height == ProfileDimension && IsFormerDefault(pixels);
    if (pixels != nullptr)
    {
        stbi_image_free(pixels);
    }
    return former && WriteDefaultProfileJpeg(destination);
}
