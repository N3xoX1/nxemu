#include "input_service.h"
#include <nxemu-core/settings/settings.h>
#include <array>
#include <algorithm>
#include <string>

namespace
{
// The order follows Native*Values, not controller UI display order. Changing
// the existing configuration keys is a migration and must not happen here.
constexpr std::array<const char *, static_cast<size_t>(NativeButtonValues::NumButtons)> kButtons = {
    "A", "B", "X", "Y", "LStick", "RStick", "L", "R", "ZL", "ZR", "Plus", "Minus",
    "DLeft", "DUp", "DRight", "DDown", "SLLeft", "SRLeft", "Home", "Screenshot",
    "SLRight", "SRRight"};
constexpr std::array<const char *, static_cast<size_t>(NativeAnalogValues::NumAnalogs)> kAnalogs = {
    "LStick", "RStick"};
constexpr std::array<const char *, static_cast<size_t>(NativeMotionValues::NumMotions)> kMotions = {
    "Left", "Right"};

const char * BindingName(HostBindingGroup group, uint32_t index)
{
    switch (group)
    {
    case HostBindingGroup::Button: return index < kButtons.size() ? kButtons[index] : nullptr;
    case HostBindingGroup::Analog: return index < kAnalogs.size() ? kAnalogs[index] : nullptr;
    case HostBindingGroup::Motion: return index < kMotions.size() ? kMotions[index] : nullptr;
    }
    return nullptr;
}

const char * GroupName(HostBindingGroup group)
{
    switch (group)
    {
    case HostBindingGroup::Button: return "Button";
    case HostBindingGroup::Analog: return "Analog";
    case HostBindingGroup::Motion: return "Motion";
    }
    return nullptr;
}

std::string PlayerKey(uint32_t player)
{
    return "player_" + std::to_string(player);
}

constexpr uint32_t kPlayerCount = 10;
constexpr size_t kMaxBindingBytes = 4096;
} // namespace

const char * HostInputService::BindingKey(HostBindingGroup group, uint32_t index)
{
    return BindingName(group, index);
}

JsonValue HostInputService::StoredPlayerOverrides(uint32_t player) const
{
    if (player >= kPlayerCount) { return {}; }
    const JsonValue section = SettingsStore::GetInstance().GetSettings("nxemu-os");
    if (!section.isObject()) { return {}; }
    const JsonValue & controllers = section["controller"];
    if (!controllers.isObject()) { return {}; }
    const JsonValue & overrides = controllers[PlayerKey(player)];
    return overrides.isObject() ? overrides : JsonValue{};
}

std::optional<std::string> HostInputService::StoredBinding(uint32_t player,
    HostBindingGroup group, uint32_t index) const
{
    const char * name = BindingKey(group, index);
    const char * category = GroupName(group);
    if (name == nullptr || category == nullptr) { return std::nullopt; }
    const JsonValue stored = StoredPlayerOverrides(player);
    if (!stored.isObject()) { return std::nullopt; }
    const JsonValue & entry = stored[category];
    if (!entry.isObject() || !entry[name].isString()) { return std::nullopt; }
    return entry[name].asString();
}

bool HostInputService::SetStoredBindingOffline(uint32_t player, HostBindingGroup group,
    uint32_t index, const std::string & serialized)
{
    // During emulation the controller/OS module is authoritative; do not
    // modify SettingsStore behind its back and then lose the edit on shutdown.
    const char * name = BindingKey(group, index);
    const char * category = GroupName(group);
    if (m_os != nullptr || player >= kPlayerCount || name == nullptr || category == nullptr ||
        serialized.size() > kMaxBindingBytes || serialized.find('\0') != std::string::npos)
    {
        return false;
    }

    SettingsStore & store = SettingsStore::GetInstance();
    JsonValue section = store.GetSettings("nxemu-os");
    if (!section.isNull() && !section.isObject()) { return false; }
    // Do not destroy unexpected data when the configuration is malformed.
    if (section.isObject() && section.isMember("controller") &&
        !section["controller"].isObject()) { return false; }
    JsonValue & playerSettings = section["controller"][PlayerKey(player)];
    if (!playerSettings.isNull() && !playerSettings.isObject()) { return false; }
    if (playerSettings.isObject() && playerSettings.isMember(category) &&
        !playerSettings[category].isObject()) { return false; }
    playerSettings[category][name] = serialized;
    store.SetSettings("nxemu-os", section);
    store.Save(); // Existing serialization and file location; no second writer format.
    return true;
}


bool HostInputService::SaveOfflinePlayers(const std::array<JsonValue, 8> & players,
                                          const std::array<bool, 8> & changed)
{
    if (m_os != nullptr) { return false; }
    if (std::none_of(changed.begin(), changed.end(), [](bool value) { return value; })) { return true; }
    JsonValue section = SettingsStore::GetInstance().GetSettings("nxemu-os");
    if (!section.isNull() && !section.isObject()) { return false; }
    if (section.isObject() && section.isMember("controller") &&
        !section["controller"].isObject()) { return false; }
    for (uint32_t player = 0; player < changed.size(); ++player)
    {
        if (!changed[player]) { continue; }
        if (!players[player].isObject()) { return false; }
        for (const char * group : {"Button", "Analog", "Motion"})
        {
            const JsonValue & bindings = players[player][group];
            if (!bindings.isNull() && !bindings.isObject()) { return false; }
            if (bindings.isObject())
            {
                for (const std::string & key : bindings.GetMemberNames())
                {
                    const JsonValue & value = bindings[key];
                    if (!value.isString() || value.asString().size() > kMaxBindingBytes ||
                        value.asString().find('\0') != std::string::npos) { return false; }
                }
            }
        }
        const JsonValue & existing = section["controller"][PlayerKey(player)];
        if (!existing.isNull() && !existing.isObject()) { return false; }
    }
    for (uint32_t player = 0; player < changed.size(); ++player)
    {
        if (changed[player])
        {
            section["controller"][PlayerKey(player)] = players[player];
        }
    }
    SettingsStore & store = SettingsStore::GetInstance();
    store.SetSettings("nxemu-os", section);
    store.Save(); // SettingsStore's existing save has no result contract.
    return true;
}
