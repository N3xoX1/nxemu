#include <algorithm>
#include <cstdio>
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
    std::map<std::string, std::string> strings, sections, gameSections;
    std::vector<Callback> callbacks;
    bool debug = true;
    const char* GetString(const char* id) const override { auto it = strings.find(id); return it != strings.end() ? it->second.c_str() : ""; }
    bool GetBool(const char* id) const override { auto it = bools.find(id); return it != bools.end() && it->second; }
    int32_t GetInt(const char* id) const override { auto it = ints.find(id); return it != ints.end() ? it->second : 0; }
    float GetFloat(const char*) const override { return 0; }
    void Notify(const char* id) { auto copy = callbacks; for (const auto& c : copy) if (c.id == id) c.call(id, c.user); }
    void SetString(const char* id, const char* v) override { strings[id] = v; Notify(id); }
    void SetBool(const char* id, bool value) override { bools[id] = value; Notify(id); }
    void SetInt(const char* id, int32_t value) override { ints[id] = value; Notify(id); }
    void SetFloat(const char*, float) override {}
    void SetDefaultBool(const char*, bool) override {}
    void SetDefaultInt(const char*, int) override {}
    void SetDefaultFloat(const char*, float) override {}
    void SetDefaultString(const char*, const char*) override {}
    const char* GetSectionSettings(const char* s) const override { auto it = sections.find(s); return it != sections.end() ? it->second.c_str() : ""; }
    void SetSectionSettings(const char* s, const char* j) override { sections[s] = j; }
    void RegisterCallback(const char* id, SettingChangeCallback call, void* user) override { callbacks.push_back({id, call, user}); }
    void UnregisterCallback(const char* id, SettingChangeCallback call, void* user) override {
        std::erase_if(callbacks, [&](const auto& c) { return c.id == id && c.call == call && c.user == user; });
    }
    void SetDebugSetting(const char*, bool) override {}
    bool DebugSettingsEnabled() const override { return debug; }
    const char* GetGameSectionSettings(const char* s) const override { auto it = gameSections.find(s); return it != gameSections.end() ? it->second.c_str() : ""; }
    void SetGameSectionSettings(const char* s, const char* j) override { gameSections[s] = j; }
} settings;
IModuleSettings* g_settings = &settings;
int failures = 0;
void Check(bool value, const char* label) { std::printf("%s %s\n", value ? "PASS" : "FAIL", label); failures += !value; }
int Acc(const char* id) { return settings.GetInt(id); }
const int Auto = (int)CpuAccuracy::Auto, Accurate = (int)CpuAccuracy::Accurate, Unsafe = (int)CpuAccuracy::Unsafe, Paranoid = (int)CpuAccuracy::Paranoid;
int main() {
    settings.sections["nxemu-cpu"] = "{\"cpu\":{\"cpu_accuracy\":\"Accurate\"}}";
    SetupCpuSetting();
    Check(Acc(NXCpuSetting::SystemCpuAccuracy) == Accurate, "system value read from config");
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Accurate, "game follows system with no title");
    Check(cpuSettings.cpu_accuracy == CpuAccuracy::Accurate, "runtime field holds game value");
    settings.SetInt(NXCpuSetting::SystemCpuAccuracy, Unsafe);
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Unsafe, "system change copies to game when not overridden");
    Check(cpuSettings.global_cpu_accuracy == CpuAccuracy::Unsafe && cpuSettings.cpu_accuracy == CpuAccuracy::Unsafe, "both fields updated");

    settings.gameSections["nxemu-cpu"] = "{\"cpu\":{\"cpu_accuracy\":\"Paranoid\"}}";
    settings.SetString(NXCoreSetting::GameTitleId, "0100000000000001");
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Paranoid, "title change loads game override");
    Check(cpuSettings.cpu_accuracy == CpuAccuracy::Paranoid, "runtime field uses override");
    settings.SetInt(NXCpuSetting::SystemCpuAccuracy, Accurate);
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Paranoid, "override survives system change");

    SaveCpuSettings();
    Check(settings.gameSections["nxemu-cpu"].find("Paranoid") != std::string::npos, "override saved to game section");
    Check(settings.sections["nxemu-cpu"].find("Accurate") != std::string::npos, "system value saved to global section");
    Check(settings.sections["nxemu-cpu"].find("Paranoid") == std::string::npos, "override not written to global section");

    settings.SetInt(NXCpuSetting::GameCpuAccuracy, Accurate);
    SaveCpuSettings();
    Check(settings.gameSections["nxemu-cpu"].empty(), "game value equal to system is not stored");
    settings.SetInt(NXCpuSetting::SystemCpuAccuracy, Auto);
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Auto, "inherited again: follows system");

    settings.SetInt(NXCpuSetting::GameCpuAccuracy, Unsafe);
    settings.debug = false;
    settings.SetBool(NXCoreSetting::DebugSettingsEnabled, false);
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Auto, "debug-locked: game JSON ignored on reload");
    settings.gameSections["nxemu-cpu"] = "{\"cpu\":{\"cpu_accuracy\":\"Paranoid\"}}";
    SaveCpuSettings();
    Check(settings.gameSections["nxemu-cpu"].find("Paranoid") != std::string::npos, "debug-locked: saved game value kept");

    settings.SetString(NXCoreSetting::GameTitleId, "");
    settings.debug = true;
    settings.gameSections["nxemu-cpu"] = "";
    settings.SetBool(NXCoreSetting::DebugSettingsEnabled, true);
    Check(Acc(NXCpuSetting::GameCpuAccuracy) == Acc(NXCpuSetting::SystemCpuAccuracy), "title cleared: game equals system");

    CleanupCpuSetting();
    Check(settings.callbacks.empty(), "all callbacks unregistered");
    return failures != 0;
}
