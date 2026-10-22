#include "base_dll.h"
#include "pe_resource_dll.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>

namespace
{
constexpr const char * kResourceType = "UI";
constexpr uint16_t kResourceLanguage = 0x400;

bool ReadFileBytes(const std::filesystem::path & path, std::vector<uint8_t> & out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

bool AddFile(const std::filesystem::path & file, const std::string & name, bool verbose, std::vector<PeResource> & resources)
{
    if (verbose)
    {
        std::cout << "Processing " << name << std::endl;
    }
    PeResource resource;
    resource.typeIsString = true;
    resource.typeName = kResourceType;
    resource.nameIsString = true;
    resource.name = name;
    resource.language = kResourceLanguage;
    if (!ReadFileBytes(file, resource.data))
    {
        std::cout << "Error: Failed to open \"" << file.string() << "\"" << std::endl;
        return false;
    }
    resources.push_back(std::move(resource));
    return true;
}

bool CollectAssets(const std::filesystem::path & assetDir, bool verbose, std::vector<PeResource> & resources)
{
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator entry(assetDir, error), end; !error && entry != end; entry.increment(error))
    {
        if (!entry->is_regular_file(error))
        {
            continue;
        }
        const std::string name = entry->path().lexically_relative(assetDir).generic_string();
        if (!AddFile(entry->path(), name, verbose, resources))
        {
            return false;
        }
    }
    return !error;
}

bool CollectCatalogues(const std::filesystem::path & langDir, bool verbose, std::vector<PeResource> & resources)
{
    std::error_code error;
    for (std::filesystem::directory_iterator entry(langDir, error), end; !error && entry != end; entry.increment(error))
    {
        if (!entry->is_regular_file(error) || entry->path().extension() != ".po")
        {
            continue;
        }
        if (!AddFile(entry->path(), "lang/" + entry->path().filename().generic_string(), verbose, resources))
        {
            return false;
        }
    }
    return !error;
}

bool WritePack(const std::filesystem::path & assetDir, const std::filesystem::path & langDir, const std::filesystem::path & targetFile, bool verbose)
{
    std::vector<PeResource> resources;
    if (!CollectAssets(assetDir, verbose, resources) || !CollectCatalogues(langDir, verbose, resources))
    {
        return false;
    }
    if (resources.empty())
    {
        std::cout << "Error: No files found in \"" << assetDir.string() << "\"" << std::endl;
        return false;
    }

    std::vector<uint8_t> image;
    if (!BuildPeResourceDll(basedll, sizeof(basedll), resources, image))
    {
        std::cout << "Error: Failed to build PE resource DLL" << std::endl;
        return false;
    }

    std::ofstream target(targetFile, std::ios::binary | std::ios::trunc);
    if (!target || !target.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size())))
    {
        std::cout << "Error: Failed to write \"" << targetFile.string() << "\"" << std::endl;
        target.close();
        std::error_code error;
        std::filesystem::remove(targetFile, error);
        return false;
    }
    return true;
}
} // namespace

int main(int argc, char * argv[])
{
    if (argc < 4)
    {
        std::cout << "Usage: " << std::filesystem::path(argv[0]).filename().string() << " <asset_dir> <lang_dir> <output_file> [-v]" << std::endl;
        return 1;
    }
    std::error_code error;
    const std::filesystem::path assetDir = std::filesystem::absolute(argv[1], error);
    const std::filesystem::path langDir = std::filesystem::absolute(argv[2], error);
    const std::filesystem::path targetFile = std::filesystem::absolute(argv[3], error);
    if (!std::filesystem::is_directory(assetDir, error))
    {
        std::cout << "Error: Asset directory does not exist" << std::endl;
        return 1;
    }
    if (!std::filesystem::is_directory(langDir, error))
    {
        std::cout << "Error: Language directory does not exist" << std::endl;
        return 1;
    }
    if (!std::filesystem::is_directory(targetFile.parent_path(), error))
    {
        std::cout << "Error: Path for the file to be generated does not exist" << std::endl;
        return 1;
    }
    const bool verbose = argc > 4 && std::strcmp(argv[4], "-v") == 0;
    return WritePack(assetDir, langDir, targetFile, verbose) ? 0 : 1;
}
