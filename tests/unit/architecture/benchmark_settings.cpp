#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>

static void Count(const char *, void *data)
{
    ++*static_cast<std::uint64_t *>(data);
}

template <class F> void Measure(const char *name, std::uint32_t iterations, F action)
{
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint32_t i = 0; i < iterations; ++i)
        checksum += action(i);
    const double elapsed =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s,%.3f,%llu\n", name, elapsed / iterations, static_cast<unsigned long long>(checksum));
}

int main(int argc, char **argv)
{
    const auto iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 2000000UL;
    if (iterations == 0 || iterations > 100000000UL)
        return 1;
    SettingsStore store;
    store.SetInt(NXCoreSetting::EmulationState, 4);
    store.SetBool(NXCoreSetting::DisplayedFrames, true);
    std::puts("operation,ns_per_iteration,checksum");
    Measure("read_status_pair", iterations, [&](auto) {
        return store.GetInt(NXCoreSetting::EmulationState) + store.GetBool(NXCoreSetting::DisplayedFrames);
    });
    Measure("write_status_no_subscriber", iterations, [&](auto i) {
        store.SetInt(NXCoreSetting::EmulationState, i & 7);
        return 0;
    });
    std::uint64_t callbacks = 0;
    store.RegisterCallback(NXCoreSetting::DisplayedFrames, Count, &callbacks);
    Measure("write_status_unrelated_subscriber", iterations, [&](auto i) {
        store.SetInt(NXCoreSetting::EmulationState, i & 7);
        return 0;
    });
    store.UnregisterCallback(NXCoreSetting::DisplayedFrames, Count, &callbacks);
    store.RegisterCallback(NXCoreSetting::EmulationState, Count, &callbacks);
    Measure("write_status_one_subscriber", iterations, [&](auto i) {
        store.SetInt(NXCoreSetting::EmulationState, i & 7);
        return 0;
    });
    store.UnregisterCallback(NXCoreSetting::EmulationState, Count, &callbacks);
    std::printf("callbacks,0,%llu\n", static_cast<unsigned long long>(callbacks));
    return callbacks == iterations ? 0 : 2;
}
