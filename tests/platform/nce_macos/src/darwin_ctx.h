// Darwin arm64 signal context helpers.
#pragma once

#include <cstdint>
#include <cstring>
#include <signal.h>
#include <sys/ucontext.h>

namespace ncev {

using Mcontext = __darwin_mcontext64;

inline Mcontext * Mctx(void * raw_context) {
    return static_cast<ucontext_t *>(raw_context)->uc_mcontext;
}

// x0..x28 live in __x, x29 is __fp and x30 is __lr.
inline std::uint64_t & Gpr(Mcontext * mc, int index) {
    if (index < 29) {
        return mc->__ss.__x[index];
    }
    return index == 29 ? mc->__ss.__fp : mc->__ss.__lr;
}

// ESR_ELx decoding of the syndrome the kernel copies to __es.__esr.
struct Esr {
    std::uint32_t raw;
    std::uint32_t Ec() const { return raw >> 26; }
    std::uint32_t Iss() const { return raw & 0x1FFFFFF; }
    std::uint32_t Fsc() const { return raw & 0x3F; }
    bool WnR() const { return ((raw >> 6) & 1) != 0; }
    bool IsDataAbort() const { return Ec() == 0x24 || Ec() == 0x25; }
    bool IsInstructionAbort() const { return Ec() == 0x20 || Ec() == 0x21; }
    bool IsAlignment() const { return Fsc() == 0x21; }
    bool IsPermission() const { return (Fsc() & 0x3C) == 0x0C; }
    bool IsTranslation() const { return (Fsc() & 0x3C) == 0x04; }
};

const char * SignalName(int sig);

} // namespace ncev
