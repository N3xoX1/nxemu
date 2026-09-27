#include "module_list.h"
#include "settings/core_settings.h"
#include <common/dynamic_library.h>
#include <common/path_finder.h>
#include <common/std_string.h>

ModuleList::ModuleList(bool autoFill) :
    m_moduleDir(coreSettings.moduleDir)
{
    if (autoFill)
    {
        LoadList();
    }
}

void ModuleList::LoadList()
{
    m_modules.clear();
    AddModuleFromDir(m_moduleDir);
}

void ModuleList::AddModuleFromDir(const Path & dir)
{
    PathFinder dirFinder(Path(dir.GetDriveDirectory().c_str(), "*"));
    Path foundDir;
    if (dirFinder.FindFirst(foundDir, PathFinder::FIND_ATTRIBUTE_SUBDIR))
    {
        do
        {
            AddModuleFromDir(foundDir);
        } while (dirFinder.FindNext(foundDir));
    }

#if defined(_WIN32)
    constexpr const char * modulePattern = "*.dll";
#elif defined(__APPLE__)
    constexpr const char * modulePattern = "*.dylib";
#else
    constexpr const char * modulePattern = "*.so";
#endif
    PathFinder fileFinder(Path(dir.GetDriveDirectory().c_str(), modulePattern));
    Path foundFile;
    if (fileFinder.FindFirst(foundFile))
    {
        DynLibHandle hLib = nullptr;
        do
        {
            if (hLib != nullptr)
            {
                DynamicLibraryClose(hLib);
                hLib = nullptr;
            }

            hLib = DynamicLibraryOpen(foundFile);
            if (hLib == nullptr)
            {
                continue;
            }

            ModuleBase::tyGetModuleInfo getModuleInfo = (ModuleBase::tyGetModuleInfo)DynamicLibraryGetProc(hLib, "GetModuleInfo");
            if (getModuleInfo == nullptr)
            {
                continue;
            }

            std::unique_ptr<MODULE> module = std::make_unique<MODULE>();
            getModuleInfo(&module->info);
            if (!ModuleBase::ValidVersion(module->info))
            {
                continue;
            }
            module->path = foundFile;
            module->file = stdstr((const char *)foundFile).substr(strlen(m_moduleDir));
            m_modules.push_back(std::move(module));
        } while (fileFinder.FindNext(foundFile));

        if (hLib != nullptr)
        {
            DynamicLibraryClose(hLib);
            hLib = nullptr;
        }
    }
}

uint32_t ModuleList::GetModuleCount() const
{
    return (uint32_t)m_modules.size();
}

const ModuleList::MODULE * ModuleList::GetModuleInfo(uint32_t indx) const
{
    return indx < m_modules.size() ? m_modules[indx].get() : nullptr;
}