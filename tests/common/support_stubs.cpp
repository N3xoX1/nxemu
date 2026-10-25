// Minimal definitions so the real nce/patcher.cpp and arm_dynarmic.cpp link without
// the rest of NXEmu.
#include <cstdio>
#include <cstdlib>

#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"

namespace Common::Log {

LogLevel g_classLevel[(uint8_t)LogClass::Count]{};

void FmtLogMessageImpl(LogClass, LogLevel, const char * filename, unsigned int line_num, const char *,
                       const char * format, const fmt::format_args & args) {
    std::fprintf(stderr, "[log %s:%u] %s\n", filename, line_num, fmt::vformat(format, args).c_str());
}

} // namespace Common::Log

void assert_fail_impl() {
    std::fprintf(stderr, "assertion failed\n");
    std::abort();
}

[[noreturn]] void unreachable_impl() {
    std::fprintf(stderr, "unreachable\n");
    std::abort();
}
