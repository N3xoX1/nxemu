#include "rom_properties.h"
#include <algorithm>
#include <nxemu-module-spec/system_loader.h>
#include <yuzu_common/fs/filesystem_interfaces.h>

namespace
{
// Reads one of IRomInfo's size-then-data properties into a container.
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
} // namespace

std::string RomProperties::Title(IRomInfo & info)
{
    std::string title = ReadProperty<std::string>([&](char * data, uint32_t * size) { return info.ReadTitle(data, size); });
    title.resize(std::min(title.find('\0'), title.size()));
    return title;
}

std::string RomProperties::ActiveUpdateVersion(IRomInfo & info)
{
    std::string update = ReadProperty<std::string>(
        [&](char * data, uint32_t * size) { return info.ReadActiveUpdateVersion(data, size); });
    update.resize(std::min(update.find('\0'), update.size()));
    return update;
}

void RomProperties::Release(IRomInfo & info, std::string & version, std::string & publisher,
                            std::string & baseVersion)
{
    IFileSysNACPPtr control(info.GetBaseControlMetadata());
    if (control)
    {
        baseVersion = control->GetVersionString();
        publisher = control->GetDeveloperName();
    }
    const std::string activeUpdate = ActiveUpdateVersion(info);
    version = activeUpdate.empty() ? baseVersion : activeUpdate;
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
