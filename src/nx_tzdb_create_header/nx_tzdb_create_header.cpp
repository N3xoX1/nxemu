#include <common/path.h>
#include <common/path_finder.h>
#include <common/std_string.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

bool processTimezoneFiles(const Path & zonePath, const std::string & headerName, const Path & outputDir, const Path & templatePath)
{
    std::vector<Path> fileList;
    PathFinder dirFinder(Path(zonePath, "*"));
    Path foundFile;
    if (dirFinder.FindFirst(foundFile, PathFinder::FIND_ATTRIBUTE_FILES))
    {
        do
        {
            fileList.push_back(foundFile);
        } while (dirFinder.FindNext(foundFile));
    }

    if (fileList.empty())
    {
        std::cerr << "No timezone files found in directory " << zonePath << std::endl;
        return false;
    }

    std::ostringstream fileData;
    for (const auto & zoneFile : fileList)
    {
        fileData << "{\"" << zoneFile.GetNameExtension() << "\",\n{";

        std::ifstream input(zoneFile, std::ios::binary);
        if (!input)
        {
            std::cerr << "Failed to open file: " << zoneFile << std::endl;
            continue;
        }

        char byte;
        int byteCount = 0;
        while (input.get(byte))
        {
            if (byteCount > 0 && byteCount % 19 == 0)
            {
                fileData << "\n";
            }
            else if (byteCount > 0)
            {
                fileData << " ";
            }

            fileData << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(byte)) << ",";
            byteCount++;
        }
        fileData << (byteCount > 0 && byteCount % 19 == 0 ? "\n}},\n" : " }},\n");
    }

    std::ifstream templateFile(templatePath);
    if (!templateFile)
    {
        std::cerr << "Failed to open template file: " << templatePath << std::endl;
        return false;
    }

    stdstr templateContent(std::string((std::istreambuf_iterator<char>(templateFile)), std::istreambuf_iterator<char>()));
    templateContent.Replace("@FILE_DATA@", fileData.str());
    templateContent.Replace("@DIRECTORY_NAME@", headerName.c_str());
    Path outputFile(outputDir, stdstr_f("%s.h",headerName.c_str()).c_str());
    outputFile.AppendDirectory("nx_tzdb");
    if (!outputFile.DirectoryExists())
    {
        outputFile.DirectoryCreate();
    }
    std::ofstream output((std::string)outputFile);
    if (!output)
    {
        std::cerr << "Failed to create output file" << std::endl;
        return false;
    }

    output << templateContent;
    std::cout << "Generated header file: " << headerName << ".h" << std::endl;
    return true;
}

int main(int argc, char * argv[])
{
    if (argc != 5)
    {
        std::cerr << "Usage: " << argv[0] << " <zone_path> <header_name> <nx_tzdb_include_dir> <nx_tzdb_source_dir>" << std::endl;
        return 1;
    }

    Path zonePath(argv[1], "");
    std::string headerName = argv[2];
    Path includeDir(argv[3], "");
    Path sourceDir(argv[4], "");
    Path templatePath(sourceDir, "tzdb_template.h.in");

    if (!zonePath.DirectoryExists() || !includeDir.DirectoryExists() || !sourceDir.DirectoryExists() || !templatePath.FileExists())
    {
        std::cerr << "Usage: " << argv[0] << " <zone_path> <header_name> <nx_tzdb_include_dir> <nx_tzdb_source_dir>" << std::endl;
        return 1;
    }
    return processTimezoneFiles(zonePath, headerName, includeDir, templatePath) ? 0 : 1;
}