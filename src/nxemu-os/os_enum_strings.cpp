#include "os_enum_strings.h"

const char* ControllerTypeToString(ControllerType value)
{
    switch (value) {
    case ControllerType::ProController: return "ProController";
    case ControllerType::DualJoyconDetached: return "DualJoyconDetached";
    case ControllerType::LeftJoycon: return "LeftJoycon";
    case ControllerType::RightJoycon: return "RightJoycon";
    case ControllerType::Handheld: return "Handheld";
    case ControllerType::GameCube: return "GameCube";
    case ControllerType::Pokeball: return "Pokeball";
    case ControllerType::NES: return "NES";
    case ControllerType::SNES: return "SNES";
    case ControllerType::N64: return "N64";
    case ControllerType::SegaGenesis: return "SegaGenesis";
    }
    return "ProController";
}

ControllerType ControllerTypeFromString(std::string_view str)
{
    if (str == "ProController") return ControllerType::ProController;
    if (str == "DualJoyconDetached") return ControllerType::DualJoyconDetached;
    if (str == "LeftJoycon") return ControllerType::LeftJoycon;
    if (str == "RightJoycon") return ControllerType::RightJoycon;
    if (str == "Handheld") return ControllerType::Handheld;
    if (str == "GameCube") return ControllerType::GameCube;
    if (str == "Pokeball") return ControllerType::Pokeball;
    if (str == "NES") return ControllerType::NES;
    if (str == "SNES") return ControllerType::SNES;
    if (str == "N64") return ControllerType::N64;
    if (str == "SegaGenesis") return ControllerType::SegaGenesis;
    return ControllerType::ProController;
}

const char* AudioEngineToString(AudioCore::Sink::AudioEngine value)
{
    switch (value) {
    case AudioCore::Sink::AudioEngine::Auto: return "auto";
    case AudioCore::Sink::AudioEngine::Cubeb: return "cubeb";
    case AudioCore::Sink::AudioEngine::Sdl2: return "sdl2";
    case AudioCore::Sink::AudioEngine::Null: return "null";
    case AudioCore::Sink::AudioEngine::Oboe: return "oboe";
    }
    return "auto";
}

AudioCore::Sink::AudioEngine AudioEngineFromString(std::string_view str)
{
    if (str == "auto") return AudioCore::Sink::AudioEngine::Auto;
    if (str == "cubeb") return AudioCore::Sink::AudioEngine::Cubeb;
    if (str == "sdl2") return AudioCore::Sink::AudioEngine::Sdl2;
    if (str == "null") return AudioCore::Sink::AudioEngine::Null;
    if (str == "oboe") return AudioCore::Sink::AudioEngine::Oboe;
    return AudioCore::Sink::AudioEngine::Auto;
}

const char* AudioModeToString(AudioMode value)
{
    switch (value) {
    case AudioMode::Mono: return "Mono";
    case AudioMode::Stereo: return "Stereo";
    case AudioMode::Surround: return "Surround";
    }
    return "Stereo";
}

AudioMode AudioModeFromString(std::string_view str)
{
    if (str == "Mono") return AudioMode::Mono;
    if (str == "Stereo") return AudioMode::Stereo;
    if (str == "Surround") return AudioMode::Surround;
    return AudioMode::Stereo;
}

const char* LanguageToString(Language value)
{
    switch (value) {
    case Language::Japanese: return "Japanese";
    case Language::EnglishAmerican: return "EnglishAmerican";
    case Language::French: return "French";
    case Language::German: return "German";
    case Language::Italian: return "Italian";
    case Language::Spanish: return "Spanish";
    case Language::Chinese: return "Chinese";
    case Language::Korean: return "Korean";
    case Language::Dutch: return "Dutch";
    case Language::Portuguese: return "Portuguese";
    case Language::Russian: return "Russian";
    case Language::Taiwanese: return "Taiwanese";
    case Language::EnglishBritish: return "EnglishBritish";
    case Language::FrenchCanadian: return "FrenchCanadian";
    case Language::SpanishLatin: return "SpanishLatin";
    case Language::ChineseSimplified: return "ChineseSimplified";
    case Language::ChineseTraditional: return "ChineseTraditional";
    case Language::PortugueseBrazilian: return "PortugueseBrazilian";
    }
    return "EnglishAmerican";
}

Language LanguageFromString(std::string_view str)
{
    if (str == "Japanese") return Language::Japanese;
    if (str == "EnglishAmerican") return Language::EnglishAmerican;
    if (str == "French") return Language::French;
    if (str == "German") return Language::German;
    if (str == "Italian") return Language::Italian;
    if (str == "Spanish") return Language::Spanish;
    if (str == "Chinese") return Language::Chinese;
    if (str == "Korean") return Language::Korean;
    if (str == "Dutch") return Language::Dutch;
    if (str == "Portuguese") return Language::Portuguese;
    if (str == "Russian") return Language::Russian;
    if (str == "Taiwanese") return Language::Taiwanese;
    if (str == "EnglishBritish") return Language::EnglishBritish;
    if (str == "FrenchCanadian") return Language::FrenchCanadian;
    if (str == "SpanishLatin") return Language::SpanishLatin;
    if (str == "ChineseSimplified") return Language::ChineseSimplified;
    if (str == "ChineseTraditional") return Language::ChineseTraditional;
    if (str == "PortugueseBrazilian") return Language::PortugueseBrazilian;
    return Language::EnglishAmerican;
}

const char* DockedModeToString(DockedMode value)
{
    switch (value) {
    case DockedMode::Handheld: return "Handheld";
    case DockedMode::Docked: return "Docked";
    }
    return "Docked";
}

DockedMode DockedModeFromString(std::string_view str)
{
    if (str == "Handheld") return DockedMode::Handheld;
    if (str == "Docked") return DockedMode::Docked;
    return DockedMode::Docked;
}

const char* MemoryLayoutToString(MemoryLayout value)
{
    switch (value) {
    case MemoryLayout::Memory_4Gb: return "Memory_4Gb";
    case MemoryLayout::Memory_6Gb: return "Memory_6Gb";
    case MemoryLayout::Memory_8Gb: return "Memory_8Gb";
    }
    return "Memory_4Gb";
}

MemoryLayout MemoryLayoutFromString(std::string_view str)
{
    if (str == "Memory_4Gb") return MemoryLayout::Memory_4Gb;
    if (str == "Memory_6Gb") return MemoryLayout::Memory_6Gb;
    if (str == "Memory_8Gb") return MemoryLayout::Memory_8Gb;
    return MemoryLayout::Memory_4Gb;
}

const char* AppletModeToString(AppletMode value)
{
    return value == AppletMode::LLE ? "LLE" : "HLE";
}

AppletMode AppletModeFromString(std::string_view str, AppletMode fallback)
{
    if (str == "HLE") return AppletMode::HLE;
    if (str == "LLE") return AppletMode::LLE;
    return fallback;
}
