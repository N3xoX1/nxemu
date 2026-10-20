#include "pe_resource_dll.h"
#include <map>

namespace
{

constexpr uint32_t IMAGE_RESOURCE_NAME_IS_STRING = 0x80000000u;
constexpr uint32_t IMAGE_RESOURCE_DATA_IS_DIRECTORY = 0x80000000u;

uint32_t AlignUp(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void AppendU16(std::vector<uint8_t> & out, uint16_t value)
{
    out.push_back((uint8_t)(value & 0xff));
    out.push_back((uint8_t)((value >> 8) & 0xff));
}

void WriteU16(std::vector<uint8_t> & out, size_t offset, uint16_t value)
{
    out[offset] = (uint8_t)(value & 0xff);
    out[offset + 1] = (uint8_t)((value >> 8) & 0xff);
}

void WriteU32(std::vector<uint8_t> & out, size_t offset, uint32_t value)
{
    out[offset] = (int8_t)(value & 0xff);
    out[offset + 1] = (uint8_t)((value >> 8) & 0xff);
    out[offset + 2] = (uint8_t)((value >> 16) & 0xff);
    out[offset + 3] = (uint8_t)((value >> 24) & 0xff);
}

uint16_t ReadU16(const uint8_t * data, size_t offset)
{
    return (uint16_t)(data[offset] | (data[offset + 1] << 8));
}

uint32_t ReadU32(const uint8_t * data, size_t offset)
{
    return (uint32_t)(data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) | (data[offset + 3] << 24));
}

struct ResourceKey
{
    bool isString = false;
    uint32_t id = 0;
    std::string name;

    bool operator<(const ResourceKey & other) const
    {
        if (isString != other.isString)
        {
            return isString && !other.isString;
        }
        if (isString)
        {
            return name < other.name;
        }
        return id < other.id;
    }
};

ResourceKey MakeTypeKey(const PeResource & resource)
{
    ResourceKey key;
    key.isString = resource.typeIsString;
    key.id = resource.typeId;
    key.name = resource.typeName;
    return key;
}

ResourceKey MakeNameKey(const PeResource & resource)
{
    ResourceKey key;
    key.isString = resource.nameIsString;
    key.id = resource.nameId;
    key.name = resource.name;
    return key;
}

uint32_t AppendUtf16String(std::vector<uint8_t> & strings, const std::string & value)
{
    const uint32_t offset = (uint32_t)strings.size();
    AppendU16(strings, (uint16_t)value.size());
    for (unsigned char ch : value)
    {
        AppendU16(strings, ch);
    }
    return offset;
}

struct BuiltSection
{
    std::vector<uint8_t> bytes;
};

using LanguageMap = std::map<uint16_t, const PeResource *>;
using NameMap = std::map<ResourceKey, LanguageMap>;
using TypeMap = std::map<ResourceKey, NameMap>;
using StringOffsetMap = std::map<ResourceKey, uint32_t>;

uint32_t InternString(std::vector<uint8_t> & stringTable, StringOffsetMap & stringOffsets, const ResourceKey & key)
{
    if (!key.isString)
    {
        return 0;
    }
    StringOffsetMap::const_iterator existing = stringOffsets.find(key);
    if (existing != stringOffsets.end())
    {
        return existing->second;
    }
    const uint32_t offset = AppendUtf16String(stringTable, key.name);
    stringOffsets.emplace(key, offset);
    return offset;
}

void WriteDirectoryHeader(std::vector<uint8_t> & section, size_t offset, uint16_t namedCount, uint16_t idCount)
{
    WriteU32(section, offset + 0, 0);
    WriteU32(section, offset + 4, 0);
    WriteU16(section, offset + 8, 0);
    WriteU16(section, offset + 10, 0);
    WriteU16(section, offset + 12, namedCount);
    WriteU16(section, offset + 14, idCount);
}

template <typename Value>
uint16_t CountNamed(const std::map<ResourceKey, Value> & map)
{
    uint16_t count = 0;
    for (const std::pair<const ResourceKey, Value> & entry : map)
    {
        if (entry.first.isString)
        {
            count++;
        }
    }
    return count;
}

bool BuildResourceSection(const std::vector<PeResource> & resources, uint32_t sectionRva, BuiltSection & outSection)
{
    TypeMap tree;
    for (const PeResource & resource : resources)
    {
        tree[MakeTypeKey(resource)][MakeNameKey(resource)][resource.language] = &resource;
    }

    if (tree.empty())
    {
        return false;
    }

    size_t directoryBytes = 16 + tree.size() * 8;
    for (const TypeMap::value_type & typeEntry : tree)
    {
        directoryBytes += 16 + typeEntry.second.size() * 8;
        for (const NameMap::value_type & nameEntry : typeEntry.second)
        {
            directoryBytes += 16 + nameEntry.second.size() * 8;
        }
    }

    size_t dataEntryCount = 0;
    for (const TypeMap::value_type & typeEntry : tree)
    {
        for (const NameMap::value_type & nameEntry : typeEntry.second)
        {
            dataEntryCount += nameEntry.second.size();
        }
    }
    const size_t dataEntriesBytes = dataEntryCount * 16;

    std::vector<uint8_t> stringTable;
    StringOffsetMap stringOffsets;
    for (const TypeMap::value_type & typeEntry : tree)
    {
        InternString(stringTable, stringOffsets, typeEntry.first);
        for (const NameMap::value_type & nameEntry : typeEntry.second)
        {
            InternString(stringTable, stringOffsets, nameEntry.first);
        }
    }

    std::vector<uint8_t> dataBlobs;
    std::vector<uint32_t> dataBlobOffsets;
    dataBlobOffsets.reserve(dataEntryCount);
    for (const TypeMap::value_type & typeEntry : tree)
    {
        for (const NameMap::value_type & nameEntry : typeEntry.second)
        {
            for (const LanguageMap::value_type & languageEntry : nameEntry.second)
            {
                const PeResource * resource = languageEntry.second;
                dataBlobs.resize(AlignUp((uint32_t)(dataBlobs.size()), 4));
                dataBlobOffsets.push_back((uint32_t)(dataBlobs.size()));
                dataBlobs.insert(dataBlobs.end(), resource->data.begin(), resource->data.end());
            }
        }
    }

    const uint32_t stringsOffset = (uint32_t)(directoryBytes + dataEntriesBytes);
    const uint32_t dataOffset = stringsOffset + (uint32_t)(stringTable.size());
    const uint32_t alignedDataOffset = AlignUp(dataOffset, 4);
    const uint32_t dataPad = alignedDataOffset - dataOffset;

    std::vector<uint8_t> section;
    section.reserve(alignedDataOffset + dataBlobs.size());
    section.resize(directoryBytes + dataEntriesBytes);

    size_t dirWrite = 0;
    size_t dataEntryWrite = directoryBytes;
    size_t dataBlobIndex = 0;

    const uint16_t rootNamed = CountNamed(tree);
    const uint16_t rootIds = (uint16_t)(tree.size() - rootNamed);
    WriteDirectoryHeader(section, dirWrite, rootNamed, rootIds);
    size_t rootEntries = dirWrite + 16;
    dirWrite += 16 + tree.size() * 8;

    size_t rootEntryIndex = 0;
    for (const TypeMap::value_type & typeEntry : tree)
    {
        const ResourceKey & typeKey = typeEntry.first;
        const uint32_t typeDirOffset = (uint32_t)(dirWrite);
        if (typeKey.isString)
        {
            WriteU32(section, rootEntries + rootEntryIndex * 8, IMAGE_RESOURCE_NAME_IS_STRING | (stringsOffset + stringOffsets[typeKey]));
        }
        else
        {
            WriteU32(section, rootEntries + rootEntryIndex * 8, typeKey.id);
        }
        WriteU32(section, rootEntries + rootEntryIndex * 8 + 4, IMAGE_RESOURCE_DATA_IS_DIRECTORY | typeDirOffset);
        rootEntryIndex++;

        const uint16_t typeNamed = CountNamed(typeEntry.second);
        const uint16_t typeIds = (uint16_t)(typeEntry.second.size() - typeNamed);
        WriteDirectoryHeader(section, dirWrite, typeNamed, typeIds);
        size_t typeEntries = dirWrite + 16;
        dirWrite += 16 + typeEntry.second.size() * 8;

        size_t typeEntryIndex = 0;
        for (const NameMap::value_type & nameEntry : typeEntry.second)
        {
            const ResourceKey & nameKey = nameEntry.first;
            const uint32_t nameDirOffset = (uint32_t)(dirWrite);
            if (nameKey.isString)
            {
                WriteU32(section, typeEntries + typeEntryIndex * 8, IMAGE_RESOURCE_NAME_IS_STRING | (stringsOffset + stringOffsets[nameKey]));
            }
            else
            {
                WriteU32(section, typeEntries + typeEntryIndex * 8, nameKey.id);
            }
            WriteU32(section, typeEntries + typeEntryIndex * 8 + 4, IMAGE_RESOURCE_DATA_IS_DIRECTORY | nameDirOffset);
            typeEntryIndex++;

            WriteDirectoryHeader(section, dirWrite, 0, (uint16_t)(nameEntry.second.size()));
            size_t langEntries = dirWrite + 16;
            dirWrite += 16 + nameEntry.second.size() * 8;

            size_t langEntryIndex = 0;
            for (const LanguageMap::value_type & languageEntry : nameEntry.second)
            {
                const PeResource * resource = languageEntry.second;
                const uint32_t dataEntryOffset = (uint32_t)(dataEntryWrite);
                WriteU32(section, langEntries + langEntryIndex * 8, languageEntry.first);
                WriteU32(section, langEntries + langEntryIndex * 8 + 4, dataEntryOffset);
                langEntryIndex++;

                const uint32_t blobOffsetInSection = alignedDataOffset + dataBlobOffsets[dataBlobIndex];
                WriteU32(section, dataEntryWrite + 0, sectionRva + blobOffsetInSection);
                WriteU32(section, dataEntryWrite + 4, (uint32_t)(resource->data.size()));
                WriteU32(section, dataEntryWrite + 8, resource->codePage);
                WriteU32(section, dataEntryWrite + 12, 0);
                dataEntryWrite += 16;
                dataBlobIndex++;
            }
        }
    }

    if (dirWrite != directoryBytes || dataEntryWrite != directoryBytes + dataEntriesBytes)
    {
        return false;
    }

    section.insert(section.end(), stringTable.begin(), stringTable.end());
    section.insert(section.end(), dataPad, 0);
    section.insert(section.end(), dataBlobs.begin(), dataBlobs.end());
    outSection.bytes = std::move(section);
    return true;
}

} // namespace

bool BuildPeResourceDll(const uint8_t * baseDll, size_t baseDllSize, const std::vector<PeResource> & resources, std::vector<uint8_t> & outImage)
{
    if (baseDll == nullptr || baseDllSize < 0x40 || resources.empty())
    {
        return false;
    }

    if (baseDll[0] != 'M' || baseDll[1] != 'Z')
    {
        return false;
    }

    const uint32_t eLfanew = ReadU32(baseDll, 0x3C);
    if (eLfanew + 24 > baseDllSize)
    {
        return false;
    }
    if (baseDll[eLfanew] != 'P' || baseDll[eLfanew + 1] != 'E')
    {
        return false;
    }

    const uint32_t coff = eLfanew + 4;
    const uint16_t numberOfSections = ReadU16(baseDll, coff + 2);
    const uint16_t sizeOfOptionalHeader = ReadU16(baseDll, coff + 16);
    const uint32_t optionalHeader = coff + 20;
    const uint16_t magic = ReadU16(baseDll, optionalHeader);
    if (magic != 0x20B) // PE32+
    {
        return false;
    }

    const uint32_t sectionAlignment = ReadU32(baseDll, optionalHeader + 32);
    const uint32_t fileAlignment = ReadU32(baseDll, optionalHeader + 36);
    const uint32_t numberOfRvaAndSizes = ReadU32(baseDll, optionalHeader + 108);
    const uint32_t dataDirectories = optionalHeader + 112;
    const uint32_t sectionTable = optionalHeader + sizeOfOptionalHeader;

    if (numberOfRvaAndSizes < 3 || sectionTable + numberOfSections * 40u > baseDllSize)
    {
        return false;
    }

    int rsrcSectionIndex = -1;
    uint32_t rsrcSectionHeader = 0;
    for (uint16_t i = 0; i < numberOfSections; ++i)
    {
        const uint32_t header = sectionTable + i * 40u;
        if (std::memcmp(baseDll + header, ".rsrc", 5) == 0)
        {
            rsrcSectionIndex = i;
            rsrcSectionHeader = header;
            break;
        }
    }
    if (rsrcSectionIndex < 0)
    {
        return false;
    }

    const uint32_t rsrcVa = ReadU32(baseDll, rsrcSectionHeader + 12);
    const uint32_t rsrcRawPtr = ReadU32(baseDll, rsrcSectionHeader + 20);

    BuiltSection resourceSection;
    if (!BuildResourceSection(resources, rsrcVa, resourceSection))
    {
        return false;
    }

    const uint32_t virtualSize = (uint32_t)(resourceSection.bytes.size());
    const uint32_t sizeOfRawData = AlignUp(virtualSize, fileAlignment);
    const uint32_t sizeOfImage = AlignUp(rsrcVa + virtualSize, sectionAlignment);

    outImage.assign(baseDll, baseDll + rsrcRawPtr);
    outImage.insert(outImage.end(), resourceSection.bytes.begin(), resourceSection.bytes.end());
    outImage.resize(rsrcRawPtr + sizeOfRawData, 0);

    uint32_t initializedData = 0;
    for (uint16_t i = 0; i < numberOfSections; ++i)
    {
        const uint32_t header = sectionTable + i * 40u;
        const uint32_t characteristics = ReadU32(outImage.data(), header + 36);
        const bool isUninit = (characteristics & 0x00000080u) != 0;
        const bool isDiscardable = (characteristics & 0x02000000u) != 0;
        if (isUninit || isDiscardable)
        {
            continue;
        }
        uint32_t rawSize = ReadU32(outImage.data(), header + 16);
        if (i == (uint16_t)(rsrcSectionIndex))
        {
            rawSize = sizeOfRawData;
        }
        initializedData += rawSize;
    }
    WriteU32(outImage, optionalHeader + 8, initializedData);
    WriteU32(outImage, optionalHeader + 56, sizeOfImage);
    WriteU32(outImage, rsrcSectionHeader + 8, virtualSize);
    WriteU32(outImage, rsrcSectionHeader + 16, sizeOfRawData);
    WriteU32(outImage, rsrcSectionHeader + 20, rsrcRawPtr);
    WriteU32(outImage, dataDirectories + 2 * 8, rsrcVa);
    WriteU32(outImage, dataDirectories + 2 * 8 + 4, virtualSize);
    WriteU32(outImage, optionalHeader + 64, 0);
    return true;
}
