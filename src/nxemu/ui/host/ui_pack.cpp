#include "ui_pack.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>
#include <yuzu_common/fs/path_util.h>

namespace
{
constexpr uint32_t IMAGE_RESOURCE_NAME_IS_STRING = 0x80000000u;
constexpr uint32_t IMAGE_RESOURCE_DATA_IS_DIRECTORY = 0x80000000u;
constexpr uint16_t IMAGE_NT_OPTIONAL_HDR32_MAGIC = 0x10B;
constexpr uint16_t IMAGE_NT_OPTIONAL_HDR64_MAGIC = 0x20B;
constexpr const char * kResourceType = "UI";

bool ReadU16(const uint8_t * data, size_t size, size_t offset, uint16_t & value)
{
    if (offset + 2 > size)
    {
        return false;
    }
    value = (uint16_t)(data[offset] | (data[offset + 1] << 8));
    return true;
}

bool ReadU32(const uint8_t * data, size_t size, size_t offset, uint32_t & value)
{
    if (offset + 4 > size)
    {
        return false;
    }
    value = (uint32_t)(data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24));
    return true;
}

std::string Upper(std::string_view text)
{
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

struct Entry
{
    uint32_t offset = 0;
    uint32_t size = 0;
};

class Pack
{
public:
    bool Load(const std::filesystem::path & file);
    const Entry * Find(const std::string & name) const;
    const uint8_t * Data(const Entry & entry) const { return m_bytes.data() + entry.offset; }

private:
    bool Parse();
    bool RvaToFileOffset(uint32_t rva, uint32_t & fileOffset) const;
    bool ResourceDirOffsetToFile(uint32_t offsetFromRsrc, uint32_t bytesNeeded, uint32_t & fileOffset) const;
    bool ReadResourceString(uint32_t stringOffset, std::string & text) const;
    bool FindTypeDirectory(uint32_t & directoryOffset) const;
    bool ReadEntryData(uint32_t nameOffsetToData, Entry & entry) const;
    bool Index();

    std::vector<uint8_t> m_bytes;
    uint32_t m_sectionTable = 0;
    uint16_t m_numberOfSections = 0;
    uint32_t m_rsrcRva = 0;
    uint32_t m_rsrcSize = 0;
    uint32_t m_rsrcFileOffset = 0;
    std::unordered_map<std::string, Entry> m_entries;
};

bool Pack::Load(const std::filesystem::path & file)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    m_bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !m_bytes.empty() && m_bytes.size() <= UINT32_MAX && Parse() && Index();
}

const Entry * Pack::Find(const std::string & name) const
{
    const auto entry = m_entries.find(Upper(name));
    return entry != m_entries.end() ? &entry->second : nullptr;
}

bool Pack::RvaToFileOffset(uint32_t rva, uint32_t & fileOffset) const
{
    const uint8_t * image = m_bytes.data();
    const size_t imageSize = m_bytes.size();
    for (uint16_t i = 0; i < m_numberOfSections; ++i)
    {
        const uint32_t header = m_sectionTable + (uint32_t)i * 40u;
        uint32_t virtualSize = 0;
        uint32_t virtualAddress = 0;
        uint32_t sizeOfRawData = 0;
        uint32_t pointerToRawData = 0;
        if (!ReadU32(image, imageSize, header + 8, virtualSize) ||
            !ReadU32(image, imageSize, header + 12, virtualAddress) ||
            !ReadU32(image, imageSize, header + 16, sizeOfRawData) ||
            !ReadU32(image, imageSize, header + 20, pointerToRawData))
        {
            return false;
        }
        const uint32_t sectionSpan = virtualSize > sizeOfRawData ? virtualSize : sizeOfRawData;
        if (sectionSpan == 0)
        {
            continue;
        }
        if (rva >= virtualAddress && (rva - virtualAddress) < sectionSpan)
        {
            const uint32_t sectionOffset = rva - virtualAddress;
            if (sectionOffset >= sizeOfRawData)
            {
                return false;
            }
            fileOffset = pointerToRawData + sectionOffset;
            return fileOffset < imageSize;
        }
    }
    return false;
}

bool Pack::Parse()
{
    const uint8_t * bytes = m_bytes.data();
    const size_t size = m_bytes.size();
    if (size < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
    {
        return false;
    }

    uint32_t eLfanew = 0;
    if (!ReadU32(bytes, size, 0x3C, eLfanew) || eLfanew + 24 > size)
    {
        return false;
    }
    if (bytes[eLfanew] != 'P' || bytes[eLfanew + 1] != 'E' || bytes[eLfanew + 2] != 0 || bytes[eLfanew + 3] != 0)
    {
        return false;
    }

    const uint32_t coff = eLfanew + 4;
    uint16_t numberOfSections = 0;
    uint16_t sizeOfOptionalHeader = 0;
    if (!ReadU16(bytes, size, coff + 2, numberOfSections) || !ReadU16(bytes, size, coff + 16, sizeOfOptionalHeader) ||
        numberOfSections == 0 || sizeOfOptionalHeader < 2)
    {
        return false;
    }

    const uint32_t optionalHeader = coff + 20;
    uint16_t magic = 0;
    if (!ReadU16(bytes, size, optionalHeader, magic))
    {
        return false;
    }

    uint32_t numberOfRvaAndSizesOffset = 0;
    uint32_t dataDirectories = 0;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        numberOfRvaAndSizesOffset = optionalHeader + 92;
        dataDirectories = optionalHeader + 96;
    }
    else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        numberOfRvaAndSizesOffset = optionalHeader + 108;
        dataDirectories = optionalHeader + 112;
    }
    else
    {
        return false;
    }

    uint32_t numberOfRvaAndSizes = 0;
    if (!ReadU32(bytes, size, numberOfRvaAndSizesOffset, numberOfRvaAndSizes) || numberOfRvaAndSizes < 3)
    {
        return false;
    }

    const uint32_t sectionTable = optionalHeader + sizeOfOptionalHeader;
    if (sectionTable + (uint32_t)numberOfSections * 40u > size)
    {
        return false;
    }

    uint32_t rsrcRva = 0;
    uint32_t rsrcSize = 0;
    if (!ReadU32(bytes, size, dataDirectories + 2 * 8, rsrcRva) || !ReadU32(bytes, size, dataDirectories + 2 * 8 + 4, rsrcSize) ||
        rsrcRva == 0 || rsrcSize < 16)
    {
        return false;
    }

    m_sectionTable = sectionTable;
    m_numberOfSections = numberOfSections;
    m_rsrcRva = rsrcRva;
    m_rsrcSize = rsrcSize;
    return RvaToFileOffset(m_rsrcRva, m_rsrcFileOffset);
}

bool Pack::ResourceDirOffsetToFile(uint32_t offsetFromRsrc, uint32_t bytesNeeded, uint32_t & fileOffset) const
{
    if (offsetFromRsrc + bytesNeeded < offsetFromRsrc || offsetFromRsrc + bytesNeeded > m_rsrcSize)
    {
        return false;
    }
    if (m_rsrcFileOffset > m_bytes.size() || (size_t)m_rsrcFileOffset + (size_t)offsetFromRsrc + (size_t)bytesNeeded > m_bytes.size())
    {
        return false;
    }
    fileOffset = m_rsrcFileOffset + offsetFromRsrc;
    return true;
}

bool Pack::ReadResourceString(uint32_t stringOffset, std::string & text) const
{
    uint32_t lengthOffset = 0;
    uint16_t length = 0;
    if (!ResourceDirOffsetToFile(stringOffset, 2, lengthOffset) || !ReadU16(m_bytes.data(), m_bytes.size(), lengthOffset, length))
    {
        return false;
    }
    uint32_t charsOffset = 0;
    if (!ResourceDirOffsetToFile(stringOffset + 2, (uint32_t)length * 2u, charsOffset))
    {
        return false;
    }
    text.clear();
    for (uint16_t i = 0; i < length; ++i)
    {
        uint16_t ch = 0;
        if (!ReadU16(m_bytes.data(), m_bytes.size(), charsOffset + (uint32_t)i * 2u, ch) || ch > 0x7F)
        {
            return false;
        }
        text.push_back(static_cast<char>(ch));
    }
    return true;
}

bool Pack::FindTypeDirectory(uint32_t & directoryOffset) const
{
    uint32_t headerOffset = 0;
    uint16_t namedCount = 0;
    if (!ResourceDirOffsetToFile(0, 16, headerOffset) || !ReadU16(m_bytes.data(), m_bytes.size(), headerOffset + 12, namedCount))
    {
        return false;
    }
    uint32_t entriesOffset = 0;
    if (!ResourceDirOffsetToFile(16, (uint32_t)namedCount * 8u, entriesOffset))
    {
        return false;
    }
    for (uint32_t i = 0; i < namedCount; ++i)
    {
        uint32_t nameOrId = 0;
        uint32_t dataOffset = 0;
        std::string name;
        if (!ReadU32(m_bytes.data(), m_bytes.size(), entriesOffset + i * 8u, nameOrId) ||
            !ReadU32(m_bytes.data(), m_bytes.size(), entriesOffset + i * 8u + 4, dataOffset))
        {
            return false;
        }
        if ((nameOrId & IMAGE_RESOURCE_NAME_IS_STRING) != 0 && (dataOffset & IMAGE_RESOURCE_DATA_IS_DIRECTORY) != 0 &&
            ReadResourceString(nameOrId & ~IMAGE_RESOURCE_NAME_IS_STRING, name) && Upper(name) == kResourceType)
        {
            directoryOffset = dataOffset & ~IMAGE_RESOURCE_DATA_IS_DIRECTORY;
            return true;
        }
    }
    return false;
}

bool Pack::ReadEntryData(uint32_t nameOffsetToData, Entry & entry) const
{
    if ((nameOffsetToData & IMAGE_RESOURCE_DATA_IS_DIRECTORY) == 0)
    {
        return false;
    }
    const uint32_t languageDirectory = nameOffsetToData & ~IMAGE_RESOURCE_DATA_IS_DIRECTORY;
    uint32_t languageHeader = 0;
    uint16_t namedCount = 0;
    uint16_t idCount = 0;
    if (!ResourceDirOffsetToFile(languageDirectory, 16, languageHeader) || !ReadU16(m_bytes.data(), m_bytes.size(), languageHeader + 12, namedCount) ||
        !ReadU16(m_bytes.data(), m_bytes.size(), languageHeader + 14, idCount) || (uint32_t)namedCount + (uint32_t)idCount == 0)
    {
        return false;
    }
    uint32_t firstLanguageEntry = 0;
    uint32_t languageDataOffset = 0;
    if (!ResourceDirOffsetToFile(languageDirectory + 16, 8, firstLanguageEntry) ||
        !ReadU32(m_bytes.data(), m_bytes.size(), firstLanguageEntry + 4, languageDataOffset) || (languageDataOffset & IMAGE_RESOURCE_DATA_IS_DIRECTORY) != 0)
    {
        return false;
    }
    uint32_t dataEntryOffset = 0;
    uint32_t dataRva = 0;
    uint32_t dataSize = 0;
    if (!ResourceDirOffsetToFile(languageDataOffset, 16, dataEntryOffset) || !ReadU32(m_bytes.data(), m_bytes.size(), dataEntryOffset, dataRva) ||
        !ReadU32(m_bytes.data(), m_bytes.size(), dataEntryOffset + 4, dataSize))
    {
        return false;
    }
    uint32_t dataFileOffset = 0;
    if (!RvaToFileOffset(dataRva, dataFileOffset) || (size_t)dataFileOffset + (size_t)dataSize > m_bytes.size())
    {
        return false;
    }
    entry.offset = dataFileOffset;
    entry.size = dataSize;
    return true;
}

bool Pack::Index()
{
    uint32_t typeDirectory = 0;
    if (!FindTypeDirectory(typeDirectory))
    {
        return false;
    }
    uint32_t headerOffset = 0;
    uint16_t namedCount = 0;
    if (!ResourceDirOffsetToFile(typeDirectory, 16, headerOffset) || !ReadU16(m_bytes.data(), m_bytes.size(), headerOffset + 12, namedCount))
    {
        return false;
    }
    uint32_t entriesOffset = 0;
    if (!ResourceDirOffsetToFile(typeDirectory + 16, (uint32_t)namedCount * 8u, entriesOffset))
    {
        return false;
    }
    for (uint32_t i = 0; i < namedCount; ++i)
    {
        uint32_t nameOrId = 0;
        uint32_t dataOffset = 0;
        std::string name;
        Entry entry;
        if (!ReadU32(m_bytes.data(), m_bytes.size(), entriesOffset + i * 8u, nameOrId) ||
            !ReadU32(m_bytes.data(), m_bytes.size(), entriesOffset + i * 8u + 4, dataOffset) || (nameOrId & IMAGE_RESOURCE_NAME_IS_STRING) == 0 ||
            !ReadResourceString(nameOrId & ~IMAGE_RESOURCE_NAME_IS_STRING, name) || !ReadEntryData(dataOffset, entry))
        {
            return false;
        }
        m_entries.emplace(Upper(name), entry);
    }
    return !m_entries.empty();
}

std::unique_ptr<Pack> g_pack;
std::filesystem::path g_root;

const Entry * FindEntry(const std::filesystem::path & path)
{
    if (!g_pack)
    {
        return nullptr;
    }
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(g_root);
    const std::string name = relative.generic_string();
    if (relative.empty() || name.rfind("..", 0) == 0)
    {
        return nullptr;
    }
    return g_pack->Find(name);
}

std::filesystem::path FromRml(const Rml::String & path)
{
    return std::filesystem::path(Common::FS::ToU8String(path));
}
} // namespace

bool UiPack::Open(const std::filesystem::path & packFile, const std::filesystem::path & root)
{
    std::unique_ptr<Pack> pack = std::make_unique<Pack>();
    if (!pack->Load(packFile))
    {
        return false;
    }
    g_pack = std::move(pack);
    g_root = root.lexically_normal();
    return true;
}

bool UiPack::ReadFile(const std::filesystem::path & path, std::string & data)
{
    if (const Entry * entry = FindEntry(path))
    {
        data.assign(reinterpret_cast<const char *>(g_pack->Data(*entry)), entry->size);
        return true;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return !stream.bad();
}

bool UiPack::Exists(const std::filesystem::path & path)
{
    std::error_code error;
    return FindEntry(path) != nullptr || std::filesystem::is_regular_file(path, error);
}

struct UiPackFileInterface::File
{
    Rml::FileHandle delegate = 0;
    const uint8_t * data = nullptr;
    size_t size = 0;
    size_t position = 0;
};

Rml::FileHandle UiPackFileInterface::Open(const Rml::String & path)
{
    auto file = std::make_unique<File>();
    if (const Entry * entry = FindEntry(FromRml(path)))
    {
        file->data = g_pack->Data(*entry);
        file->size = entry->size;
    }
    else
    {
        file->delegate = m_delegate.Open(path);
        if (file->delegate == 0)
        {
            return 0;
        }
    }
    return reinterpret_cast<Rml::FileHandle>(file.release());
}

void UiPackFileInterface::Close(Rml::FileHandle handle)
{
    std::unique_ptr<File> file(reinterpret_cast<File *>(handle));
    if (file->delegate)
    {
        m_delegate.Close(file->delegate);
    }
}

size_t UiPackFileInterface::Read(void * buffer, size_t size, Rml::FileHandle handle)
{
    File & file = *reinterpret_cast<File *>(handle);
    if (file.delegate)
    {
        return m_delegate.Read(buffer, size, file.delegate);
    }
    const size_t count = std::min(size, file.size - file.position);
    if (count != 0)
    {
        std::memcpy(buffer, file.data + file.position, count);
    }
    file.position += count;
    return count;
}

bool UiPackFileInterface::Seek(Rml::FileHandle handle, long offset, int origin)
{
    File & file = *reinterpret_cast<File *>(handle);
    if (file.delegate)
    {
        return m_delegate.Seek(file.delegate, offset, origin);
    }
    if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END)
    {
        return false;
    }
    const int64_t base = static_cast<int64_t>(origin == SEEK_CUR ? file.position : origin == SEEK_END ? file.size : 0);
    const int64_t position = base + offset;
    if (position < 0 || static_cast<uint64_t>(position) > file.size)
    {
        return false;
    }
    file.position = static_cast<size_t>(position);
    return true;
}

size_t UiPackFileInterface::Tell(Rml::FileHandle handle)
{
    File & file = *reinterpret_cast<File *>(handle);
    return file.delegate ? m_delegate.Tell(file.delegate) : file.position;
}

size_t UiPackFileInterface::Length(Rml::FileHandle handle)
{
    File & file = *reinterpret_cast<File *>(handle);
    return file.delegate ? m_delegate.Length(file.delegate) : file.size;
}
