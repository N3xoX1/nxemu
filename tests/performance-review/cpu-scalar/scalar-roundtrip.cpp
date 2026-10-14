// SPDX-License-Identifier: GPL-2.0-or-later
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;
using Vector = Dynarmic::A64::Vector;

struct Callbacks final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit{};
    std::vector<u32> code;
    std::array<unsigned char, 4096> memory{};
    unsigned svc_calls{};

    std::optional<u32> MemoryReadCode(u64 address) override {
        if (address < 0x1000 || (address - 0x1000) / 4 >= code.size()) return {};
        return code[(address - 0x1000) / 4];
    }
    template <typename T> T Read(u64 address) {
        if (address > memory.size() - sizeof(T)) throw std::runtime_error("RAM read bounds");
        T value;
        std::memcpy(&value, memory.data() + address, sizeof(T));
        return value;
    }
    template <typename T> void Write(u64 address, T value) {
        if (address > memory.size() - sizeof(T)) throw std::runtime_error("RAM write bounds");
        std::memcpy(memory.data() + address, &value, sizeof(T));
    }
    uint8_t MemoryRead8(u64 a) override { return Read<uint8_t>(a); }
    uint16_t MemoryRead16(u64 a) override { return Read<uint16_t>(a); }
    u32 MemoryRead32(u64 a) override { return Read<u32>(a); }
    u64 MemoryRead64(u64 a) override { return Read<u64>(a); }
    Vector MemoryRead128(u64 a) override { return Read<Vector>(a); }
    void MemoryWrite8(u64 a, uint8_t v) override { Write(a, v); }
    void MemoryWrite16(u64 a, uint16_t v) override { Write(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { Write(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { Write(a, v); }
    void MemoryWrite128(u64 a, Vector v) override { Write(a, v); }
    void InterpreterFallback(u64, size_t) override { throw std::runtime_error("unexpected fallback"); }
    void CallSVC(u32) override { ++svc_calls; jit->HaltExecution(); }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override { throw std::runtime_error("guest exception"); }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1'000'000'000; }
    u64 GetCNTPCT() override { return 0; }
};

int main(int argc, char** argv) try {
    if (argc != 3) throw std::runtime_error("args: state-output.bin safe|auto");
    // Synthetic A64 programs: scalar FP, integer transfers, every vector lane,
    // shared vector consumers, FP compares, and scalar/vector RAM round-trips.
    const std::vector<std::vector<u32>> programs{
        {0x1e222820, 0x1e230842, 0x1e244863, 0x1e21c084, 0x1e202805, 0x1e260006, 0x1e2700c7},
        {0x1e622820, 0x1e630842, 0x1e644863, 0x1e61c084, 0x1e602805},
        {0x0e043c01, 0x0e0c3c02, 0x0e143c03, 0x0e1c3c04, 0x4e083c05, 0x4e183c06, 0x1e270020},
        {0x4e208402, 0x1e222820, 0x0e043c03, 0x4e208404, 0x1e270061},
        {0x1e222820, 0x0e0c3c05, 0x0e143c06, 0x0e1c3c07, 0x0e043c08, 0x1e270101},
        {0x1e222820, 0x1e260006, 0x110004c7, 0x1e2700e8, 0x1e212909},
        {0x1e222820, 0xbd000140, 0xbd400141, 0x1e202820, 0x3d800140, 0x3dc00142},
        {0x1e222820, 0x1e212000, 0x1e212010, 0x1e21c003, 0x1e232864},
    };
    const u64 special[]{0, 1, 0x80000000, 0x7f800000, 0xff800000, 0x7fc12345,
        0x7f812345, 0x007fffff, 0x00800000, 0x3f800000, 0x8000000000000000,
        0x7ff0000000000000, 0x7ff8123456789abc, 0x7ff0123456789abc, ~u64{0}};
    Callbacks callbacks;
    Dynarmic::A64::UserConfig config{};
    config.callbacks = &callbacks;
    config.enable_cycle_counting = false;
    config.wall_clock_cntpct = true;
    config.code_cache_size = 32 * 1024 * 1024;
    if (std::strcmp(argv[2], "auto") == 0) {
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    } else if (std::strcmp(argv[2], "safe") != 0) {
        throw std::runtime_error("unknown accuracy");
    }
    Dynarmic::A64::Jit jit(config);
    callbacks.jit = &jit;
    std::ofstream output(argv[1], std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    std::mt19937_64 random(0x2937281);
    unsigned cases{};
    for (const auto& program : programs) {
        callbacks.code = program;
        callbacks.code.push_back(0xd4000001); // SVC #0 terminates only this program.
        jit.ClearCache();
        for (unsigned i = 0; i < 4096; ++i) {
            jit.Reset();
            jit.SetPC(0x1000);
            // Reset clears the pending invalidation flag; invalidate after Reset.
            if (i == 0) jit.ClearCache();
            std::array<Vector, 32> vectors;
            for (auto& v : vectors) for (auto& word : v)
                word = i < 256 ? special[(i + random() % 4) % std::size(special)] : random();
            jit.SetVectors(vectors);
            std::array<u64, 31> registers;
            for (auto& r : registers) r = random();
            registers[10] = 128;
            jit.SetRegisters(registers);
            for (unsigned j = 0; j < callbacks.memory.size(); ++j)
                callbacks.memory[j] = static_cast<unsigned char>(i ^ j);
            jit.SetFpcr(((i % 4) << 22) | ((i & 4) ? 1u << 24 : 0) | ((i & 8) ? 1u << 25 : 0));
            jit.SetFpsr(i & 0x9f);
            jit.SetPstate((i % 16) << 28);
            const unsigned before = callbacks.svc_calls;
            jit.Run();
            if (callbacks.svc_calls != before + 1 || jit.GetPC() != 0x1000 + callbacks.code.size() * 4)
                throw std::runtime_error("did not execute to own terminal SVC");
            vectors = jit.GetVectors();
            registers = jit.GetRegisters();
            const u32 flags[]{jit.GetFpcr(), jit.GetFpsr(), jit.GetPstate()};
            output.write(reinterpret_cast<const char*>(vectors.data()), sizeof(vectors));
            output.write(reinterpret_cast<const char*>(registers.data()), sizeof(registers));
            output.write(reinterpret_cast<const char*>(flags), sizeof(flags));
            output.write(reinterpret_cast<const char*>(callbacks.memory.data() + 128), 128);
            ++cases;
        }
    }
    std::printf("PASS %u synthetic A64 cases; compare all vector/GPR/FP state and RAM bytes\n", cases);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL %s\n", error.what());
    return 1;
}
