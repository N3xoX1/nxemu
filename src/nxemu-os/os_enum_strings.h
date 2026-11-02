#pragma once

#include <nxemu-module-spec/input.h>
#include <nxemu-module-spec/operating_system.h>
#include <string_view>
#include <yuzu_audio_core/audio_types.h>

#include "os_types.h"

const char* ControllerTypeToString(ControllerType value);
ControllerType ControllerTypeFromString(std::string_view str);

const char* AudioEngineToString(AudioCore::Sink::AudioEngine value);
AudioCore::Sink::AudioEngine AudioEngineFromString(std::string_view str);

const char* AudioModeToString(AudioMode value);
AudioMode AudioModeFromString(std::string_view str);

const char* LanguageToString(Language value);
Language LanguageFromString(std::string_view str);

const char* DockedModeToString(DockedMode value);
DockedMode DockedModeFromString(std::string_view str);

const char* MemoryLayoutToString(MemoryLayout value);
MemoryLayout MemoryLayoutFromString(std::string_view str);

const char* AppletModeToString(AppletMode value);
AppletMode AppletModeFromString(std::string_view str, AppletMode fallback);
