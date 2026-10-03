#include "file_dialogs.h"

#include <common/path.h>

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstring>
#include <string>

namespace
{

NSString * ToNSString(const char * text)
{
    if (text == nullptr || *text == '\0')
    {
        return nil;
    }
    return [NSString stringWithUTF8String:text];
}

void SetInitialDirectory(NSOpenPanel * panel, const char * initialDir)
{
    NSString * path = ToNSString(initialDir);
    if (path == nil)
    {
        return;
    }

    BOOL isDirectory = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&isDirectory] && isDirectory)
    {
        panel.directoryURL = [NSURL fileURLWithPath:path isDirectory:YES];
    }
}

NSArray<UTType *> * ContentTypesFromFilter(const char * fileFilter)
{
    if (fileFilter == nullptr || *fileFilter == '\0')
    {
        return nil;
    }

    const char * pattern = fileFilter + std::strlen(fileFilter) + 1;
    if (*pattern == '\0')
    {
        return nil;
    }

    NSMutableArray<UTType *> * types = [NSMutableArray array];
    std::string patterns(pattern);
    size_t start = 0;

    while (start <= patterns.size())
    {
        const size_t end = patterns.find(';', start);
        std::string entry = patterns.substr(start, end == std::string::npos ? std::string::npos : end - start);

        if (entry == "*" || entry == "*.*")
        {
            return nil;
        }

        if (entry.rfind("*.", 0) == 0)
        {
            entry.erase(0, 2);
        }
        else if (!entry.empty() && entry.front() == '.')
        {
            entry.erase(0, 1);
        }

        if (!entry.empty() && entry.find_first_of("*?") == std::string::npos)
        {
            NSString * extension = [NSString stringWithUTF8String:entry.c_str()];
            if (extension != nil)
            {
                UTType * type = [UTType typeWithFilenameExtension:extension];
                if (type != nil && ![types containsObject:type])
                {
                    [types addObject:type];
                }
            }
        }

        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }

    return types.count == 0 ? nil : types;
}

} // namespace

namespace MacOSFileDialogs
{

bool FileSelect(const char * initialDir, const char * fileFilter, Path & selected)
{
    @autoreleasepool
    {
        NSOpenPanel * panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.resolvesAliases = YES;

        SetInitialDirectory(panel, initialDir);
        NSArray<UTType *> * contentTypes = ContentTypesFromFilter(fileFilter);
        if (contentTypes != nil)
        {
            panel.allowedContentTypes = contentTypes;
        }

        if ([panel runModal] != NSModalResponseOK)
        {
            return false;
        }

        NSURL * url = panel.URL;
        if (url == nil || !url.fileURL)
        {
            return false;
        }

        selected = Path(url.path.fileSystemRepresentation);
        return true;
    }
}

Path BrowseForDirectory(const char * title)
{
    @autoreleasepool
    {
        NSOpenPanel * panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = YES;
        panel.resolvesAliases = YES;

        NSString * message = ToNSString(title);
        if (message != nil)
        {
            panel.message = message;
        }

        if ([panel runModal] != NSModalResponseOK)
        {
            return {};
        }

        NSURL * url = panel.URL;
        if (url == nil || !url.fileURL)
        {
            return {};
        }

        return Path(url.path.fileSystemRepresentation, "");
    }
}

} // namespace MacOSFileDialogs
