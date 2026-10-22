#include <common/file.h>
#include <common/path.h>
#include <common/sha256.h>
#include <common/std_string.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <time.h>

bool GitCommand(const Path & sourceDirectory, const char * command, std::string & output)
{
    Path currentDir(Path::CURRENT_DIRECTORY);
    if (currentDir != sourceDirectory)
    {
        sourceDirectory.DirectoryChange();
    }
    output.clear();
    FILE * pipe = _popen(stdstr_f("git %s", command).c_str(), "r");
    if (pipe == nullptr)
    {
        if (currentDir != sourceDirectory)
        {
            currentDir.DirectoryChange();
        }
        return false;
    }

    char buffer[128];
    while (!feof(pipe))
    {
        if (fgets(buffer, 128, pipe) != NULL)
        {
            output += buffer;
        }
    }
    if (currentDir != sourceDirectory)
    {
        currentDir.DirectoryChange();
    }
    if (feof(pipe))
    {
        _pclose(pipe);
        return true;
    }
    return false;
}

uint32_t GitBuildVersion(const Path & sourceDirectory)
{
    enum
    {
        DefaultBuildVersion = 9999
    };
    std::string result;
    if (!GitCommand(sourceDirectory, "rev-list --count HEAD", result))
    {
        return DefaultBuildVersion;
    }
    if (result.empty())
    {
        return DefaultBuildVersion;
    }
    uint32_t BuildVersion = atoi(result.c_str());
    if (BuildVersion != 0)
    {
        return BuildVersion;
    }
    return 9999;
}

bool GitBuildDirty(Path & sourceDirectory)
{
    std::string result;
    if (!GitCommand(sourceDirectory, "diff --stat", result))
    {
        return false;
    }
    return !result.empty();
}

std::string GitRevision(Path & sourceDirectory)
{
    stdstr result;
    if (!GitCommand(sourceDirectory, "rev-parse HEAD", result))
    {
        return "";
    }
    result.Replace("\r", "");
    strvector ResultVector = result.Tokenize("\n");
    if (ResultVector.size() > 0)
    {
        return ResultVector[0];
    }
    return "";
}

std::string GitRevisionShort(Path & sourceDirectory)
{
    stdstr result;
    if (!GitCommand(sourceDirectory, "rev-parse --short HEAD", result))
    {
        return "";
    }
    result.Replace("\r", "");
    strvector ResultVector = result.Tokenize("\n");
    if (ResultVector.size() > 0)
    {
        return ResultVector[0];
    }
    return "";
}

int main()
{
    if (__argc < 3)
    {
        return 0;
    }

    Path sourceFile(__argv[1]), destFile(__argv[2]);
    if (!sourceFile.FileExists())
    {
        return 0;
    }

    std::string destHash;
    if (destFile.FileExists())
    {
        File outFile(destFile, IFile::OpenFlags::modeRead);
        if (outFile.IsOpen())
        {
            uint32_t OutFileLen = (uint32_t)outFile.GetLength();
            std::string data;
            data.resize(OutFileLen);
            if (outFile.Read((void *)data.data(), OutFileLen) != 0)
            {
                destHash = sha256(data.c_str());
            }
        }
    }

    Path sourceDirectory(sourceFile.GetDriveDirectory(), "");
    uint32_t versionBuild = GitBuildVersion(sourceDirectory);
    bool BuildDirty = GitBuildDirty(sourceDirectory);
    std::string Revision = GitRevision(sourceDirectory);
    std::string RevisionShort = GitRevisionShort(sourceDirectory);

    File inFile(sourceFile, IFile::modeRead);
    if (!inFile.IsOpen())
    {
        return 0;
    }
    inFile.SeekToBegin();
    uint32_t fileLen = (uint32_t)inFile.GetLength();
    std::unique_ptr<uint8_t[]> inputData = std::make_unique<uint8_t[]>(fileLen);
    inFile.Read(inputData.get(), fileLen);
    strvector versionData = stdstr(std::string((char *)inputData.get(), fileLen)).Replace("\r", "").Tokenize("\n");

    uint32_t versionMajor = 0, versionMinor = 0, versionRevison = 0;
    std::string versionPrefix, lineFeed = "\r\n";
    for (size_t i = 0, n = versionData.size(); i < n; i++)
    {
        stdstr line = versionData[i];
        line.Trim();

        if (_strnicmp(line.c_str(), "VersionMajor = ", 13) == 0)
        {
            versionMajor = atoi(&(line.c_str()[14]));
        }
        else if (_strnicmp(line.c_str(), "VersionMinor = ", 13) == 0)
        {
            versionMinor = atoi(&(line.c_str()[14]));
        }
        else if (_strnicmp(line.c_str(), "VersionRevison = ", 17) == 0)
        {
            versionRevison = atoi(&(line.c_str()[17]));
        }
        else if (_strnicmp(line.c_str(), "VersionPrefix = ", 15) == 0)
        {
            size_t startPrefix = line.find("\"", 15);
            size_t endPrefix = std::string::npos;
            if (startPrefix != std::string::npos)
            {
                endPrefix = line.find("\"", startPrefix + 1);
            }
            if (endPrefix != std::string::npos)
            {
                versionPrefix = line.substr(startPrefix + 1, endPrefix - (startPrefix + 1));
            }
        }
        else
        {
            continue;
        }
        versionData.erase(versionData.begin() + i);
        i -= 1;
        n -= 1;
    }

    std::string OutData;
    for (size_t i = 0, n = versionData.size(); i < n; i++)
    {
        stdstr & line = versionData[i];
        line += lineFeed;
        if (_strnicmp(line.c_str(), "#define GIT_VERSION ", 20) == 0)
        {
            line.Format("#define GIT_VERSION                 \"%s%s%s\"%s", RevisionShort.c_str(), BuildDirty ? "-" : "", BuildDirty ? "Dirty" : "", lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "#define VERSION_BUILD ", 22) == 0)
        {
            line.Format("#define VERSION_BUILD               %d%s", versionBuild, lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "#define VERSION_BUILD_YEAR ", 27) == 0)
        {
            time_t now = time(NULL);
            struct tm * tnow = gmtime(&now);

            line.Format("#define VERSION_BUILD_YEAR          %d%s", tnow->tm_year + 1900, lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "#define GIT_REVISION ", 21) == 0)
        {
            line.Format("#define GIT_REVISION                \"%s\"%s", Revision.c_str(), lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "#define GIT_REVISION_SHORT ", 26) == 0)
        {
            line.Format("#define GIT_REVISION_SHORT          \"%s\"%s", RevisionShort.c_str(), lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "#define GIT_DIRTY ", 11) == 0)
        {
            line.Format("#define GIT_DIRTY                   \"%s\"%s", BuildDirty ? "Dirty" : "", lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "        versionCode = ", 22) == 0)
        {
            line.Format("        versionCode = %d%s", versionBuild, lineFeed.c_str());
        }
        else if (_strnicmp(line.c_str(), "        versionName = ", 22) == 0)
        {
            line.Format("        versionName = \"%d.%d.%d.%d", versionMajor, versionMinor, versionRevison, versionBuild);
            if (versionPrefix.length() > 0)
            {
                line += stdstr_f(" (%s)", versionPrefix.c_str());
            }
            line += "\"" + lineFeed;
        }
        OutData += line.c_str();
    }

    std::string outHash = sha256(OutData);
    if (outHash != destHash)
    {
        if (destFile.FileExists() && !destFile.FileDelete())
        {
            return 0;
        }

        File outFile(destFile, IFile::modeWrite | IFile::modeCreate);
        if (!outFile.IsOpen())
        {
            return 0;
        }
        outFile.Write(OutData.c_str(), (uint32_t)OutData.length());
    }
    return 0;
}
