#include "path.h"
#include "std_string.h"

#ifdef _WIN32
#include <Windows.h>
#include <io.h>
#else
#include <cerrno>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
const char * DRIVE_DELIMITER = ":";
const char * const DIR_DOUBLEDELIM = "\\\\";
const char * DIRECTORY_DELIMITER = "\\";
const char * DIRECTORY_DELIMITER2 = "/";
#else
const char * DRIVE_DELIMITER = "";
const char * const DIR_DOUBLEDELIM = "//";
const char * DIRECTORY_DELIMITER = "/";
const char * DIRECTORY_DELIMITER2 = "\\";
#endif

const char EXTENSION_DELIMITER = '.';
void * Path::m_hInst = nullptr;

Path::Path()
{
}

Path::Path(const Path & path)
{
    m_path = path.m_path;
}

Path::Path(const Path & path, const char * fileName)
{
    SetDriveDirectory(path.m_path.c_str());
    SetNameExtension(fileName);
}

Path::Path(const char * path)
{
    m_path = path ? path : "";
    CleanPath(m_path);
}

Path::Path(const char * path, const char * fileName)
{
    SetDriveDirectory(path);
    SetNameExtension(fileName);
}

Path::Path(const std::string & path)
{
    m_path = path;
    CleanPath(m_path);
}

Path::Path(const std::string & path, const char * fileName)
{
    SetDriveDirectory(path.c_str());
    SetNameExtension(fileName);
}

Path::Path(const std::string & path, const std::string & fileName)
{
    SetDriveDirectory(path.c_str());
    SetNameExtension(fileName.c_str());
}

Path::Path(DIR_CURRENT_DIRECTORY /*sdt*/, const char * nameExten)
{
    SetToCurrentDirectory();
    if (nameExten)
    {
        SetNameExtension(nameExten);
    }
}

Path::Path(DIR_MODULE_DIRECTORY /*sdt*/, const char * nameExten)
{
    SetToModuleDirectory();
    SetNameExtension(nameExten ? nameExten : "");
}

Path::~Path() = default;

Path & Path::operator=(const Path & path)
{
    if (this != &path)
    {
        m_path = path.m_path;
    }
    return *this;
}

Path::operator const char *() const
{
    return (const char *)m_path.c_str();
}

Path::operator const std::string &()
{
    return m_path;
}

void Path::GetDriveDirectory(std::string & driveDirectory) const
{
    std::string drive, directory;
    GetComponents(&drive, &directory);
#ifdef _WIN32
    driveDirectory = drive;
    if (!drive.empty())
    {
        driveDirectory += DRIVE_DELIMITER;
    }
    driveDirectory += directory;
#else
    driveDirectory = directory;
#endif
}

std::string Path::GetDriveDirectory(void) const
{
    std::string driveDirectory;
    GetDriveDirectory(driveDirectory);
    return driveDirectory;
}

void Path::GetDirectory(std::string & directory) const
{
    GetComponents(nullptr, &directory);
}

std::string Path::GetDirectory(void) const
{
    std::string directory;
    GetDirectory(directory);
    return directory;
}

void Path::GetNameExtension(std::string & nameExtension) const
{
    std::string name, extension;
    GetComponents(nullptr, nullptr, &name, &extension);
    nameExtension = name;
    if (!extension.empty())
    {
        nameExtension += EXTENSION_DELIMITER;
        nameExtension += extension;
    }
}

std::string Path::GetNameExtension(void) const
{
    std::string nameExtension;
    GetNameExtension(nameExtension);
    return nameExtension;
}

void Path::GetExtension(std::string & extension) const
{
    GetComponents(nullptr, nullptr, nullptr, &extension);
}

std::string Path::GetExtension(void) const
{
    std::string extension;
    GetExtension(extension);
    return extension;
}

void Path::GetComponents(std::string * drive, std::string * directory, std::string * name, std::string * extension) const
{
    std::string drivePart, dirPart, namePart, extPart;
    const char * basePath = m_path.c_str();
#ifdef _WIN32
    const char * driveDelim = strrchr(basePath, DRIVE_DELIMITER[0]);
    if (driveDelim != nullptr)
    {
        drivePart.assign(basePath, driveDelim - basePath);
        basePath = driveDelim + 1;
    }
#endif

    const char * lastSep = strrchr(basePath, DIRECTORY_DELIMITER[0]);
    if (lastSep != nullptr)
    {
        if (lastSep > basePath)
        {
            dirPart.assign(basePath, lastSep - basePath);
        }
        else
        {
            dirPart = DIRECTORY_DELIMITER;
        }
        namePart = lastSep + 1;
    }
    else
    {
        namePart = basePath;
    }

    const char * ext = strrchr(namePart.c_str(), '.');
    if (ext != nullptr)
    {
        extPart = ext + 1;
        namePart.resize(ext - namePart.c_str());
    }

    if (drive)
    {
        *drive = drivePart;
    }
    if (directory)
    {
        *directory = dirPart;
    }
    if (name)
    {
        *name = namePart;
    }
    if (extension)
    {
        *extension = extPart;
    }
}

bool Path::IsRelative() const
{
#ifdef _WIN32
    if (m_path.length() > 1 && m_path[1] == DRIVE_DELIMITER[0])
    {
        return false;
    }
    if (m_path.length() > 2 && m_path[0] == DIRECTORY_DELIMITER[0] && m_path[1] == DIRECTORY_DELIMITER[0])
    {
        return false;
    }
#else
    if (!m_path.empty() && m_path[0] == DIRECTORY_DELIMITER[0])
    {
        return false;
    }
#endif
    return true;
}

void Path::SetDriveDirectory(const char * driveDirectory)
{
    std::string driveDirectoryStr = driveDirectory;
    if (driveDirectoryStr.length() > 0)
    {
        EnsureTrailingSeparator(driveDirectoryStr);
        CleanPath(driveDirectoryStr);
    }

    std::string name, extension;
    GetComponents(nullptr, nullptr, &name, &extension);
    SetComponents(nullptr, driveDirectoryStr.c_str(), name.c_str(), extension.c_str());
}

void Path::SetDirectory(const char * directory, bool ensureAbsolute)
{
    std::string newDirectory = directory;
    if (ensureAbsolute)
    {
        EnsureLeadingSeparator(newDirectory);
    }
    if (newDirectory.length() > 0)
    {
        EnsureTrailingSeparator(newDirectory);
    }
    std::string drive, name, extension;
    GetComponents(&drive, nullptr, &name, &extension);
    SetComponents(drive.c_str(), newDirectory.c_str(), name.c_str(), extension.c_str());
}

void Path::SetNameExtension(const char * nameExtension)
{
    std::string directory, drive;
    GetComponents(&drive, &directory);
    SetComponents(drive.c_str(), directory.c_str(), nameExtension, nullptr);
}

void Path::SetComponents(const char * drive, const char * directory, const char * name, const char * extension)
{
    if (directory == nullptr || strlen(directory) == 0)
    {
        static char emptyDir[] = {DIRECTORY_DELIMITER[0], '\0'};
        directory = emptyDir;
    }

    std::string result;
#ifdef _WIN32
    if (drive != nullptr && strlen(drive) > 0)
    {
        result += drive;
        result += DRIVE_DELIMITER;
    }
#endif
    result += directory;
    if (!result.empty() && result.back() != DIRECTORY_DELIMITER[0])
    {
        result += DIRECTORY_DELIMITER;
    }
    if (name != nullptr && strlen(name) > 0)
    {
        result += name;
        if (extension != nullptr && strlen(extension) > 0)
        {
            result += EXTENSION_DELIMITER;
            result += extension;
        }
    }
    m_path = result;
}

void Path::AppendDirectory(const char * subDirectory)
{
    std::string newSubDirectory = subDirectory;
    if (newSubDirectory.empty())
    {
        return;
    }

    StripLeadingSeparator(newSubDirectory);
    EnsureTrailingSeparator(newSubDirectory);

    std::string drive, directory, name, extension;
    GetComponents(&drive, &directory, &name, &extension);
    EnsureTrailingSeparator(directory);
    directory += newSubDirectory;
    SetComponents(drive.c_str(), directory.c_str(), name.c_str(), extension.c_str());
}

bool Path::FileDelete(bool evenIfReadOnly) const
{
#ifdef _WIN32
    std::wstring filePath = stdstr(m_path).ToUTF16();
    uint32_t attributes = ::GetFileAttributes(filePath.c_str());
    if (attributes == (uint32_t)-1)
    {
        return false;
    }

    if (((attributes & FILE_ATTRIBUTE_READONLY) == FILE_ATTRIBUTE_READONLY) && !evenIfReadOnly)
    {
        return false;
    }

    SetFileAttributes(filePath.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFile(filePath.c_str()) != 0;
#else
    struct stat st;
    if (stat(m_path.c_str(), &st) != 0)
    {
        return false;
    }

    // Check write permission if evenIfReadOnly is false
    if (!evenIfReadOnly && access(m_path.c_str(), W_OK) != 0)
    {
        return false;
    }

    // If read-only, add write permission before deleting
    if (evenIfReadOnly && access(m_path.c_str(), W_OK) != 0)
    {
        chmod(m_path.c_str(), st.st_mode | S_IWUSR);
    }

    return unlink(m_path.c_str()) == 0;
#endif 
}

bool Path::FileExists() const
{
#ifdef _WIN32
    WIN32_FIND_DATA FindData;
    HANDLE hFindFile = FindFirstFile(stdstr(m_path).ToUTF16().c_str(), &FindData);
    bool bSuccess = (hFindFile != INVALID_HANDLE_VALUE);

    if (hFindFile != nullptr) // Make sure we close the search
    {
        FindClose(hFindFile);
    }

    return bSuccess;
#else
    struct stat st;
    return stat(m_path.c_str(), &st) == 0;
#endif 
}

bool Path::IsDirectory() const
{
    std::string fileName;
    GetNameExtension(fileName);
    return fileName.empty();
}

bool Path::DirectoryCreate(bool createIntermediates)
{
    if (DirectoryExists())
    {
        return true;
    }

    std::string path;
    GetDriveDirectory(path);
    StripTrailingSeparator(path);
#ifdef _WIN32
    bool bSuccess = ::CreateDirectory(stdstr(path).ToUTF16().c_str(), nullptr) != 0;
#else
    bool bSuccess = mkdir(path.c_str(), 0755) == 0;
#endif
    if (!bSuccess && createIntermediates)
    {
        std::string::size_type delimiter = path.rfind(DIRECTORY_DELIMITER[0]);
        if (delimiter == std::string::npos)
        {
            return false;
        }

        path.resize(delimiter + 1);
        return Path(path).DirectoryCreate() ? DirectoryCreate(false) : false;
    }
    return bSuccess;
}

bool Path::DirectoryChange() const
{
    std::string driveDirectory;
    GetDriveDirectory(driveDirectory);

#ifdef _WIN32
    return SetCurrentDirectory(stdstr(driveDirectory).ToUTF16().c_str()) != 0;
#else
    return chdir(driveDirectory.c_str()) == 0;
#endif
}

bool Path::DirectoryExists() const
{
#ifdef _WIN32
    Path path(m_path.c_str());

    std::string directory;
    path.DirectoryUp(&directory);
    path.SetNameExtension(directory.c_str());

    WIN32_FIND_DATA FindData;
    HANDLE findFile = FindFirstFile(stdstr((const char *)path).ToUTF16().c_str(), &FindData);
    bool res = (findFile != INVALID_HANDLE_VALUE);
    if (findFile != INVALID_HANDLE_VALUE)
    {
        FindClose(findFile);
    }
    return res;
#else
    std::string dirPath;
    GetDriveDirectory(dirPath);
    StripTrailingSeparator(dirPath);
    if (dirPath.empty())
    {
        dirPath = ".";
    }

    struct stat st;
    if (stat(dirPath.c_str(), &st) != 0)
    {
        return false;
    }
    return S_ISDIR(st.st_mode);
#endif 
}

Path & Path::DirectoryNormalize(Path BaseDir)
{
    stdstr directory = BaseDir.GetDriveDirectory();
    bool changed = false;
    if (IsRelative())
    {
        EnsureTrailingSeparator(directory);
        directory += GetDirectory();
        changed = true;
    }
    strvector parts = directory.Tokenize(DIRECTORY_DELIMITER[0]);
    strvector normalizesParts;
    for (strvector::const_iterator itr = parts.begin(); itr != parts.end(); itr++)
    {
        if (*itr == ".")
        {
            changed = true;
        }
        else if (*itr == ".." && !normalizesParts.empty())
        {
            normalizesParts.pop_back();
            changed = true;
        }
        else
        {
            normalizesParts.push_back(*itr);
        }
    }
    if (changed)
    {
        directory.clear();
#ifndef _WIN32
        if (BaseDir.m_path.size() > 0 && BaseDir.m_path[0] == DIRECTORY_DELIMITER[0])
        {
            directory = DIRECTORY_DELIMITER;
        }
#endif
        for (strvector::const_iterator itr = normalizesParts.begin(); itr != normalizesParts.end(); itr++)
        {
            directory += *itr + DIRECTORY_DELIMITER;
        }
        SetDriveDirectory(directory.c_str());
    }
    return *this;
}

void Path::DirectoryUp(std::string * lastDir)
{
    std::string directory;
    GetDirectory(directory);
    StripTrailingSeparator(directory);
    if (directory.empty())
    {
        return;
    }
    std::string::size_type delimiter = directory.rfind(DIRECTORY_DELIMITER[0]);
    if (lastDir != nullptr)
    {
        *lastDir = directory.substr(delimiter);
        StripLeadingSeparator(*lastDir);
    }

    if (delimiter != std::string::npos)
    {
        directory = directory.substr(0, delimiter);
    }
    SetDirectory(directory.c_str());
}

void Path::SetToCurrentDirectory()
{
#ifdef _WIN32
    DWORD required = ::GetCurrentDirectory(0, nullptr);
    if (required == 0)
    {
        m_path = "";
        return;
    }
    std::vector<wchar_t> path(required);
    DWORD result = ::GetCurrentDirectory(required, path.data());
    if (result == 0 || result > required)
    {
        m_path = "";
        return;
    }
    SetDriveDirectory(stdstr().FromUTF16(path.data()).c_str());
#else
    size_t bufferSize = 4096;
    std::vector<char> buf(bufferSize);

    while (true)
    {
        if (getcwd(buf.data(), bufferSize) != nullptr)
        {
            SetDriveDirectory(buf.data());
            return;
        }
        if (errno != ERANGE)
        {
            m_path = "";
            return;
        }
        bufferSize *= 2;
        buf.resize(bufferSize);
    }
#endif 
}

void Path::SetToModuleDirectory()
{
#ifdef _WIN32
    DWORD bufferSize = MAX_PATH;
    std::vector<wchar_t> buffPath(bufferSize);

    while (true)
    {
        DWORD result = GetModuleFileNameW((HINSTANCE)m_hInst, buffPath.data(), bufferSize);

        if (result == 0)
        {
            m_path = "";
            return;
        }

        if (result < bufferSize)
        {
            m_path = stdstr().FromUTF16(buffPath.data());
            SetNameExtension("");
            return;
        }

        bufferSize *= 2;
        buffPath.resize(bufferSize);
    }
#else
    size_t bufferSize = 4096;
    std::vector<char> buffPath(bufferSize);

    while (true)
    {
        ssize_t result = readlink("/proc/self/exe", buffPath.data(), bufferSize);

        if (result < 0)
        {
            m_path = "";
            return;
        }

        if ((size_t)result < bufferSize)
        {
            m_path.assign(buffPath.data(), (size_t)result);
            SetNameExtension("");
            return;
        }

        bufferSize *= 2;
        buffPath.resize(bufferSize);
    }
#endif 
}

void Path::CleanPath(std::string & path) const
{
    std::string::size_type pos = path.find(DIRECTORY_DELIMITER2[0]);
    while (pos != std::string::npos)
    {
        path.replace(pos, 1, DIRECTORY_DELIMITER);
        pos = path.find(DIRECTORY_DELIMITER2[0], pos + 1);
    }

#ifdef _WIN32
    bool appendEnd = !_strnicmp(path.c_str(), DIR_DOUBLEDELIM, 2);
#else
    bool appendEnd = false;
#endif
    pos = path.find(DIR_DOUBLEDELIM);
    while (pos != std::string::npos)
    {
        path.replace(pos, 2, DIRECTORY_DELIMITER);
        pos = path.find(DIR_DOUBLEDELIM, pos + 1);
    }
#ifdef _WIN32
    if (appendEnd)
    {
        path.insert(0, stdstr_f("%c", DIRECTORY_DELIMITER[0]).c_str());
    }
#endif
}

void Path::EnsureLeadingSeparator(std::string & directory) const
{
    if (directory.empty() || (directory[0] != DIRECTORY_DELIMITER[0]))
    {
        directory = stdstr_f("%c%s", DIRECTORY_DELIMITER[0], directory.c_str());
    }
}

void Path::EnsureTrailingSeparator(std::string & directory) const
{
    std::string::size_type length = directory.length();

    if (directory.empty() || (directory[length - 1] != DIRECTORY_DELIMITER[0]))
    {
        directory += DIRECTORY_DELIMITER;
    }
}

void Path::StripLeadingSeparator(std::string & directory) const
{
    if (directory.length() <= 1)
    {
        return;
    }

    if (directory[0] == DIRECTORY_DELIMITER[0])
    {
        directory = directory.substr(1);
    }
}

void Path::StripTrailingSeparator(std::string & directory) const
{
    for (;;)
    {
        std::string::size_type length = directory.length();
        if (length <= 1)
        {
            break;
        }

        if (directory[length - 1] == DIRECTORY_DELIMITER[0] || directory[length - 1] == DIRECTORY_DELIMITER2[0])
        {
            directory.resize(length - 1);
            continue;
        }
        break;
    }
}
