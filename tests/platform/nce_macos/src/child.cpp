// Child process modes: checks that may crash, or need a fresh process.
#include <csignal>
#include <cstdlib>
#include <libkern/OSCacheControl.h>
#include <cstdio>
#include <cstring>
#include <mach/mach.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "nce/patcher.h"
#include "probes.h"
#include "suite.h"

namespace ncev {

namespace {

void PrintAndExitOnSigill(int) {
    std::printf("sigill\n");
    std::fflush(stdout);
    _exit(0);
}

} // namespace

int RunChildMode(const std::string & mode) {
    if (mode == "page_size") {
        void * p = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        const int r = mprotect(static_cast<char *>(p) + 0x1000, 0x1000, PROT_READ);
        std::printf("page=%lu mprotect4k=%s\n", (unsigned long)vm_page_size, r == 0 ? "ok" : "refused");
        return 0;
    }
    if (mode == "cntfrq_patch") {
        // mrs x0, cntfrq_el0 inside a module text: the real patcher hits UNREACHABLE().
        std::vector<std::uint32_t> text(0x1000, 0xD503201F);
        text[16] = 0xD53BE000;
        Core::NCE::Patcher patcher;
        patcher.PatchText(reinterpret_cast<std::uint8_t *>(text.data()), text.size() * 4, 0, text.size() * 4);
        std::printf("patched\n");
        return 0;
    }
    if (mode == "raw_svc") {
        // An unpatched guest "svc #imm" with x16 = SYS_getpid (20).
        std::printf("pid=%d svc0=%llu svc26=%llu svc7f=%llu\n", getpid(), (unsigned long long)ncev_raw_svc_0(20),
                    (unsigned long long)ncev_raw_svc_26(20), (unsigned long long)ncev_raw_svc_7f(20));
        return 0;
    }
    if (mode.rfind("insn:", 0) == 0) {
        // Executes one instruction word with x0 = a 16 KiB aligned scratch buffer.
        const std::uint32_t word = static_cast<std::uint32_t>(std::strtoul(mode.c_str() + 5, nullptr, 16));
        std::signal(SIGILL, PrintAndExitOnSigill);
        auto * code = static_cast<std::uint32_t *>(
            mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
        code[0] = word;
        code[1] = 0xD65F03C0; // ret
        mprotect(code, 0x4000, PROT_READ | PROT_EXEC);
        sys_icache_invalidate(code, 0x4000);
        void * scratch = mmap(nullptr, 0x4000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        std::memset(scratch, 0xFF, 0x4000);
        auto fn = reinterpret_cast<std::uint64_t (*)(void *)>(code);
        const std::uint64_t x0 = fn(scratch);
        int zeroed = 0;
        for (int i = 0; i < 0x4000; i++) {
            zeroed += static_cast<unsigned char *>(scratch)[i] == 0;
        }
        std::printf("ok x0=%#llx zeroed=%d\n", (unsigned long long)x0, zeroed);
        return 0;
    }
    if (mode == "x18") {
        const X18Survival r = MeasureX18();
        std::printf("%s : %s\n", r.Preserved() ? "x18 preserved" : "x18 cleared", X18Text(r).c_str());
        return 0;
    }
    if (mode == "cntpct") {
        std::signal(SIGILL, PrintAndExitOnSigill);
        const std::uint64_t a = ncev_read_cntpct();
        const std::uint64_t b = ncev_read_cntvct();
        std::printf("cntpct=%llu cntvct=%llu\n", (unsigned long long)a, (unsigned long long)b);
        return 0;
    }
    std::fprintf(stderr, "mode inconnu %s\n", mode.c_str());
    return 1;
}

} // namespace ncev
