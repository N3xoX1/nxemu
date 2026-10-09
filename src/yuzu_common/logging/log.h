// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <string_view>

#include "yuzu_common/format.h"
#include "yuzu_common/logging/formatter.h"
#include <nxemu-module-spec/base.h>

namespace Common::Log {

extern LogLevel g_classLevel[(uint8_t)LogClass::Count];

// trims up to and including the last of ../, ..\, src/, src\ in a string
constexpr const char* TrimSourcePath(std::string_view source) {
    const auto rfind = [source](const std::string_view match) {
        return source.rfind(match) == source.npos ? 0 : (source.rfind(match) + match.size());
    };
    auto idx = std::max({rfind("src/"), rfind("src\\"), rfind("../"), rfind("..\\")});
    return source.data() + idx;
}

/// Logs a message to the global logger, using fmt
void FmtLogMessageImpl(LogClass log_class, LogLevel log_level, const char * filename,
                       unsigned int line_num, const char* function, const char* format,
                       const fmt::format_args& args);

template <typename... Args>
void FmtLogMessage(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_num,
                   const char* function, const char* format, const Args&... args) {
    FmtLogMessageImpl(log_class, log_level, filename, line_num, function, format,
                      fmt::make_format_args(args...));
}

} // namespace Common::Log

#ifdef _DEBUG
#define LOG_TRACE(log_class, ...)                                                                       \
    if (Common::Log::g_classLevel[(uint8_t)LogClass::log_class] <= LogLevel::Trace) [[unlikely]] {      \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Trace,                                \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
#else
#define LOG_TRACE(log_class, fmt, ...) (void(0))
#endif

#define LOG_DEBUG(log_class, ...)                                                                       \
    if (Common::Log::g_classLevel[(uint8_t)LogClass::log_class] <= LogLevel::Debug) [[unlikely]] {      \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Debug,                                \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
#define LOG_INFO(log_class, ...)                                                                        \
    if (Common::Log::g_classLevel[(uint8_t)LogClass::log_class] <= LogLevel::Info) [[unlikely]] {       \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Info,                                 \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
#define LOG_WARNING(log_class, ...)                                                                     \
    if (Common::Log::g_classLevel[(uint8_t)LogClass::log_class] <= LogLevel::Warning) [[unlikely]] {    \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Warning,                              \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
#define LOG_ERROR(log_class, ...)                                                                       \
    if (Common::Log::g_classLevel[((uint8_t)LogClass::log_class)] <= LogLevel::Error) [[unlikely]] {    \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Error,                                \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
#define LOG_CRITICAL(log_class, ...)                                                                    \
    if (Common::Log::g_classLevel[((uint8_t)LogClass::log_class)] <= LogLevel::Critical) [[unlikely]] { \
        Common::Log::FmtLogMessage(LogClass::log_class, LogLevel::Critical,                             \
                               Common::Log::TrimSourcePath(__FILE__), __LINE__, __func__,               \
                               __VA_ARGS__);                                                            \
    }
