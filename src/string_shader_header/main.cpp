#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

void replaceAll(std::string & str, const std::string & from, const std::string & to)
{
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

int main(int argc, char ** argv)
{
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << " <source_file> <header_file> <output_file>" << std::endl;
        return 1;
    }

    const std::string source_file = argv[1];
    const std::string header_file = argv[2];
    const std::string output_file = argv[3];

    std::ifstream sourceStream(source_file);
    if (!sourceStream)
    {
        std::cerr << "Failed to open source file: " << source_file << std::endl;
        return 1;
    }

    std::string sourceContents;
    {
        std::stringstream buffer;
        buffer << sourceStream.rdbuf();
        sourceStream.close();

        sourceContents = buffer.str();    
    }
    replaceAll(sourceContents, "\\\n", ";");

    size_t pos = 0;
    std::string formattedContent;
    std::string delimiter = "\n";
    for (;;)
    {
        size_t endPos = sourceContents.find(delimiter, pos);
        std::string line = sourceContents.substr(pos, endPos - pos);
        if (!formattedContent.empty())
        {
            //formattedContent += " ";
        }
        formattedContent += "R\"(" + line + "\n)\" ";
        if (endPos == std::string::npos)
        {
            break;
        }
        pos = endPos + delimiter.length();
    }
    sourceContents = formattedContent;

    std::ifstream headerStream(header_file);
    if (!headerStream)
    {
        std::cerr << "Failed to open header file: " << header_file << std::endl;
        return 1;
    }

    std::filesystem::path sourcePath(source_file);
    std::string contents_name = sourcePath.filename().string();
    std::replace(contents_name.begin(), contents_name.end(), '.', '_');
    std::transform(contents_name.begin(), contents_name.end(), contents_name.begin(), [](unsigned char c) { return (char)std::toupper(c); });

    std::string contents;
    {
        std::stringstream buffer;
        buffer << headerStream.rdbuf();
        headerStream.close();

        contents = buffer.str();
    }
    std::string placeholder = "@CONTENTS_NAME@";
    pos = 0;
    while ((pos = contents.find(placeholder, pos)) != std::string::npos)
    {
        contents.replace(pos, placeholder.length(), contents_name);
        pos += contents_name.length();
    }

    placeholder = "@CONTENTS@";
    pos = 0;
    while ((pos = contents.find(placeholder, pos)) != std::string::npos)
    {
        contents.replace(pos, placeholder.length(), sourceContents);
        pos += sourceContents.length();
    }

    std::ofstream outputStream(output_file);
    if (!outputStream)
    {
        std::cerr << "Failed to open output file: " << output_file << std::endl;
        return 1;
    }

    outputStream << contents;
    outputStream.close();

    return 0;
}
