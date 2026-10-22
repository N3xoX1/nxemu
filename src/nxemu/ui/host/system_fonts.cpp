#include "system_fonts.h"
#include <algorithm>
#include <string>
#if defined(__APPLE__)
#include <CoreText/CoreText.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#elif defined(_WIN32)
#include <windows.h>
#elif defined(NXEMU_HAVE_FONTCONFIG)
#include <fontconfig/fontconfig.h>
#endif

namespace
{
void Add(std::vector<SystemFont> & fonts, std::filesystem::path path, int faceIndex = 0)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) return;
    if (std::none_of(fonts.begin(), fonts.end(), [&](const SystemFont & font) {
        return font.path == path && font.faceIndex == faceIndex;
    })) fonts.push_back({std::move(path), faceIndex});
}

#if defined(__APPLE__)
std::string Utf8(CFStringRef value)
{
    if (value == nullptr) return {};
    std::string result(CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1, '\0');
    if (!CFStringGetCString(value, result.data(), result.size(), kCFStringEncodingUTF8)) return {};
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

// Core Text chooses the face; FreeType finds that same face in its TTC file.
int FaceIndex(FT_Library library, const std::filesystem::path & path, const std::string & name)
{
    FT_Face face = nullptr;
    if (FT_New_Face(library, path.c_str(), 0, &face) != 0) return -1;
    const long count = face->num_faces;
    FT_Done_Face(face);
    for (long index = 0; index < count; ++index)
    {
        if (FT_New_Face(library, path.c_str(), index, &face) != 0) continue;
        const char * postscript = FT_Get_Postscript_Name(face);
        const bool matches = postscript != nullptr && name == postscript;
        FT_Done_Face(face);
        if (matches) return static_cast<int>(index);
    }
    return 0;
}

void AddMacFont(std::vector<SystemFont> & fonts, FT_Library library, CTFontRef font)
{
    if (font == nullptr) return;
    CFTypeRef location = CTFontCopyAttribute(font, kCTFontURLAttribute);
    if (location != nullptr && CFGetTypeID(location) == CFURLGetTypeID())
    {
        CFStringRef file = CFURLCopyFileSystemPath(static_cast<CFURLRef>(location), kCFURLPOSIXPathStyle);
        CFStringRef name = CTFontCopyPostScriptName(font);
        const std::filesystem::path path = Utf8(file);
        const int index = FaceIndex(library, path, Utf8(name));
        if (index >= 0) Add(fonts, path, index);
        if (name != nullptr) CFRelease(name);
        if (file != nullptr) CFRelease(file);
    }
    if (location != nullptr) CFRelease(location);
}
#endif
} // namespace

std::vector<SystemFont> SystemFallbackFonts()
{
    std::vector<SystemFont> fonts;
#if defined(__APPLE__)
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library) != 0) return fonts;
    // Some macOS UI faces use an Apple-only file format which FreeType cannot
    // read. Ask Core Text for installed public faces instead, without assuming
    // their location. Reject silent font substitution when a name is absent.
    for (CFStringRef name : {CFSTR("ArialMT"), CFSTR("HiraginoSansGB-W3"), CFSTR("HiraginoSans-W3"), CFSTR("AppleGothic")})
    {
        CTFontRef font = CTFontCreateWithName(name, 14, nullptr);
        if (font == nullptr) continue;
        CFStringRef actualName = CTFontCopyPostScriptName(font);
        if (actualName != nullptr && CFEqual(name, actualName)) AddMacFont(fonts, library, font);
        if (actualName != nullptr) CFRelease(actualName);
        CFRelease(font);
    }
    FT_Done_FreeType(library);
#elif defined(_WIN32)
    wchar_t windows[MAX_PATH + 1] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH + 1);
    if (length != 0 && length <= MAX_PATH)
    {
        const auto directory = std::filesystem::path(windows) / L"Fonts";
        for (const wchar_t * file : {L"arial.ttf", L"msyh.ttc", L"msjh.ttc", L"YuGothM.ttc", L"meiryo.ttc", L"malgun.ttf", L"msgothic.ttc", L"simsun.ttc", L"gulim.ttc"})
            Add(fonts, directory / file);
    }
#elif defined(NXEMU_HAVE_FONTCONFIG)
    FcConfig * config = FcInitLoadConfigAndFonts();
    if (config != nullptr)
    {
        for (const char * language : {"ru", "zh-cn", "zh-tw", "ja", "ko"})
        {
            FcPattern * pattern = FcPatternCreate();
            if (pattern == nullptr) continue;
            FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>("sans-serif"));
            FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>(language));
            FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);
            FcConfigSubstitute(config, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);
            FcResult result;
            FcPattern * match = FcFontMatch(config, pattern, &result);
            if (match != nullptr)
            {
                FcChar8 * file = nullptr;
                int index = 0;
                FcPatternGetInteger(match, FC_INDEX, 0, &index);
                if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                    Add(fonts, reinterpret_cast<const char *>(file), index);
                FcPatternDestroy(match);
            }
            FcPatternDestroy(pattern);
        }
        FcConfigDestroy(config);
    }
#elif defined(__ANDROID__)
    for (const char * file : {"Roboto-Regular.ttf", "NotoSansCJK-Regular.ttc", "NotoSansSC-Regular.otf", "NotoSansJP-Regular.otf", "NotoSansKR-Regular.otf"})
        Add(fonts, std::filesystem::path("/system/fonts") / file);
#endif
    return fonts;
}
