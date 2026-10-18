#include "rom_properties.h"
#include <algorithm>
#include <nxemu-module-spec/system_loader.h>

namespace
{
template <typename Container, typename Reader>
Container ReadProperty(Reader && read)
{
    Container value;
    uint32_t size = 0;
    if (read(nullptr, &size) != LoaderResultStatus::Success || size == 0)
    {
        return value;
    }
    value.resize(size);
    if (read(value.data(), &size) != LoaderResultStatus::Success)
    {
        value.clear();
        return value;
    }
    value.resize(std::min<size_t>(size, value.size()));
    return value;
}

std::string ReadText(IRomInfo & info, LoaderResultStatus (IRomInfo::*read)(char *, uint32_t *))
{
    std::string text = ReadProperty<std::string>([&](char * data, uint32_t * size) { return (info.*read)(data, size); });
    text.resize(std::min(text.find('\0'), text.size()));
    return text;
}
} // namespace

std::string RomProperties::Title(IRomInfo & info)
{
    return ReadText(info, &IRomInfo::ReadTitle);
}

std::string RomProperties::Version(IRomInfo & info)
{
    return ReadText(info, &IRomInfo::ReadDisplayVersion);
}

std::vector<uint8_t> RomProperties::Icon(IRomInfo & info)
{
    return ReadProperty<std::vector<uint8_t>>([&](uint8_t * data, uint32_t * size) { return info.ReadIcon(data, size); });
}

std::vector<uint8_t> RomProperties::Logo(IRomInfo & info)
{
    return ReadProperty<std::vector<uint8_t>>([&](uint8_t * data, uint32_t * size) { return info.ReadLogo(data, size); });
}

std::vector<uint8_t> RomProperties::Banner(IRomInfo & info)
{
    return ReadProperty<std::vector<uint8_t>>([&](uint8_t * data, uint32_t * size) { return info.ReadBanner(data, size); });
}
