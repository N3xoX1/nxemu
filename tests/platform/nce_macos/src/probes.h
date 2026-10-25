// Host-level probes (arm64_probes.S) and the fault fix-up handler for them.
#pragma once

#include <cstdint>
#include <mach/mach.h>

extern "C" {
std::uint64_t ncev_x18_spin(std::uint64_t value, std::uint64_t iterations);
std::uint64_t ncev_x18_syscall(std::uint64_t value);
std::uint64_t ncev_x18_signal(std::uint64_t value, mach_port_t self_thread);
std::uint64_t ncev_tpidr_spin(std::uint64_t value, std::uint64_t iterations);
std::uint64_t ncev_read_tpidr();
std::uint64_t ncev_read_tsd_slot(std::uint32_t byte_offset);
std::uint64_t ncev_read_tpidrro();
std::uint64_t ncev_raw_svc_0(std::uint64_t x16);
std::uint64_t ncev_raw_svc_26(std::uint64_t x16);
std::uint64_t ncev_raw_svc_7f(std::uint64_t x16);
std::uint64_t ncev_read_cntfrq();
std::uint64_t ncev_read_cntvct();
std::uint64_t ncev_read_cntpct();
std::uint64_t ncev_read_dczid();
std::uint64_t ncev_read_ctr();
void ncev_excl_add(std::uint64_t * p, std::uint64_t n);
void ncev_excl_add_ordered(std::uint64_t * p, std::uint64_t n);
void ncev_mp_write(std::uint64_t * data, std::uint64_t * flag, std::uint64_t v, int ordered);
std::uint64_t ncev_mp_read(std::uint64_t * data, std::uint64_t * flag, std::uint64_t * out, int ordered);
int ncev_probe_load(const void * addr, std::uint64_t * out);
int ncev_probe_store(void * addr, std::uint64_t value);
int ncev_probe_ldxr(void * addr);
std::uint64_t ncev_probe_call(void * fn);
}

namespace ncev {

struct ProbeFault {
    int sig = 0;
    int si_code = 0;
    std::uint64_t si_addr = 0;
    std::uint64_t far = 0;
    std::uint32_t esr = 0;
    std::uint64_t pc = 0;
};

// Must be installed before RuntimeInit, so the runtime chains host faults to it.
void InstallProbeHandler();
void SetProbeExecTarget(std::uint64_t target);
ProbeFault LastProbeFault();
void ClearProbeFault();

} // namespace ncev
