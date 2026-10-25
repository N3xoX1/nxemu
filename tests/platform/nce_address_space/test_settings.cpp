// Exercises the production CPU settings and their synchronous metadata callbacks.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>
#include "nxemu-module-spec/base.h"
#include "nxemu-core/settings/identifiers.h"
#include "nxemu-cpu/cpu_settings.h"
#include "nxemu-cpu/cpu_settings_identifiers.h"

struct TestSettings final : IModuleSettings {
    struct Callback { std::string id; SettingChangeCallback call; void* user; };
    std::map<std::string, bool> bools;
    std::map<std::string, int32_t> ints;
    std::vector<Callback> callbacks;
    const char* GetString(const char*) const override { return ""; }
    bool GetBool(const char* id) const override {
        const auto it = bools.find(id);
        return it != bools.end() && it->second;
    }
    int32_t GetInt(const char* id) const override {
        const auto it = ints.find(id);
        return it != ints.end() ? it->second : 0;
    }
    float GetFloat(const char*) const override { return 0; }
    void SetString(const char*, const char*) override {}
    void Notify(const char* id) {
        for (const auto& c : callbacks) if (c.id == id) c.call(id, c.user);
    }
    void SetBool(const char* id, bool value) override { bools[id] = value; Notify(id); }
    void SetInt(const char* id, int32_t value) override { ints[id] = value; Notify(id); }
    void SetFloat(const char*, float) override {}
    void SetDefaultBool(const char*, bool) override {}
    void SetDefaultInt(const char*, int) override {}
    void SetDefaultFloat(const char*, float) override {}
    void SetDefaultString(const char*, const char*) override {}
    const char* GetSectionSettings(const char*) const override { return ""; }
    void SetSectionSettings(const char*, const char*) override {}
    void RegisterCallback(const char* id, SettingChangeCallback call, void* user) override {
        callbacks.push_back({id, call, user});
    }
    void UnregisterCallback(const char* id, SettingChangeCallback call, void* user) override {
        std::erase_if(callbacks, [&](const auto& c) { return c.id == id && c.call == call && c.user == user; });
    }
} settings;
IModuleSettings* g_settings = &settings;
extern void UpdateNceEnabled();
int failures = 0;
void Check(bool value, const char* label) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    failures += !value;
}
bool Enabled() { return settings.GetBool(NXCpuSetting::NceEnabled); }
int main(int argc, char**) {
    SetupCpuSetting();
    Check(settings.GetInt(NXCpuSetting::CpuBackend) == static_cast<int>(CpuBackend::Nce),
          "NCE selected by default on a supported build");
    Check(!Enabled(), "metadata absent: Dynarmic");
    settings.SetBool(NXCoreSetting::Has39BitAddressSpace, true);
    Check(!Enabled(), "AArch32 even with 39-bit metadata: Dynarmic");
    settings.SetBool(NXCoreSetting::Is64BitProgram, true);
    if (argc > 1) {
        Check(!Enabled(), "current SDK without x18 preservation: Dynarmic");
        CleanupCpuSetting();
        return failures != 0;
    }
    Check(Enabled(), "AArch64 / 39 bits: NCE");
    settings.SetBool(NXCoreSetting::Has39BitAddressSpace, false);
    Check(!Enabled(), "AArch64 / 32 bits: Dynarmic");
    settings.SetBool(NXCoreSetting::Has36BitAddressSpace, true);
    unsetenv("NXEMU_NCE_36BIT"); UpdateNceEnabled();
    Check(!Enabled(), "AArch64 / 36 bits without experimental opt-in: Dynarmic");
    setenv("NXEMU_NCE_36BIT", "0", 1); UpdateNceEnabled();
    Check(!Enabled(), "experimental option set to zero: Dynarmic");
    setenv("NXEMU_NCE_36BIT", "1", 1); UpdateNceEnabled();
    Check(Enabled(), "AArch64 / 36 bits with experimental opt-in: NCE");
    settings.SetBool(NXCoreSetting::Is64BitProgram, false);
    Check(!Enabled(), "AArch32 / 36 bits with experimental opt-in: Dynarmic");
    settings.SetBool(NXCoreSetting::Is64BitProgram, true);
    settings.SetBool(NXCpuSetting::CpuDebugMode, true);
    settings.SetBool(NXCpuSetting::CpuoptFastmem, false);
    Check(!Enabled(), "fastmem disabled: Dynarmic");
    settings.SetBool(NXCpuSetting::CpuoptFastmem, true);
    Check(Enabled(), "fastmem restored: NCE");
    settings.SetInt(NXCpuSetting::CpuBackend, static_cast<int>(CpuBackend::Dynarmic));
    Check(!Enabled(), "explicit Dynarmic selection respected");
    settings.SetBool(NXCoreSetting::Is64BitProgram, false);
    settings.SetBool(NXCoreSetting::Has36BitAddressSpace, false);
    settings.SetBool(NXCoreSetting::Has39BitAddressSpace, false);
    Check(!Enabled(), "process stop clears eligibility");
    CleanupCpuSetting();
    Check(settings.callbacks.empty(), "all callbacks unregistered");
    return failures != 0;
}
