#include "game_config_addons.h"
#include "game_config.h"
#include "user_interface/html_utils.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>

namespace
{

bool IsVersionedUpdateKey(const std::string & key)
{
    return key.compare(0, 7, "Update@") == 0;
}

SciterElement FindAddonsRow(SciterElement element)
{
    SciterElement current = element;
    while (current.IsValid())
    {
        if (current.GetAttribute("class").find("addons-row") != std::string::npos)
        {
            return current;
        }
        current = current.GetParent();
    }
    return SciterElement();
}

SciterElement FindRowCheckbox(SciterElement element)
{
    if (!element.IsValid())
    {
        return SciterElement();
    }

    if (element.GetAttribute("class").find("addon-enabled") != std::string::npos)
    {
        return element;
    }

    SciterElement row = FindAddonsRow(element);
    if (!row.IsValid())
    {
        return SciterElement();
    }
    return row.FindFirst("input.addon-enabled");
}

void ToggleCheckbox(SciterElement checkbox)
{
    const bool checked = (checkbox.GetState() & SciterElement::STATE_CHECKED) != 0;
    if (checked)
    {
        checkbox.SetState(0, SciterElement::STATE_CHECKED, true);
    }
    else
    {
        checkbox.SetState(SciterElement::STATE_CHECKED, 0, true);
    }
}

} // namespace

GameConfigAddons::GameConfigAddons(ISciterUI & sciterUI, GameConfig & config, SystemModules & modules, IRomInfo & romInfo, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_modules(modules),
    m_romInfo(romInfo),
    m_page(page)
{
    PopulateAddons();

    ISystemloader & loader = m_modules.Modules().Systemloader();
    const uint64_t programId = m_config.ProgramId();
    const uint32_t count = m_romInfo.GetGamePatches(nullptr, 0);
    std::vector<GamePatchInfo> patches(count);
    if (count > 0)
    {
        m_romInfo.GetGamePatches(patches.data(), count);
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const char * key = patches[i].key[0] != '\0' ? patches[i].key : patches[i].name;
        if (key[0] != '\0' && loader.IsAddonDisabled(programId, key))
        {
            m_originalDisabled.emplace_back(key);
        }
    }
}

void GameConfigAddons::PopulateAddons(void)
{
    SciterElement list = m_page.GetElementByID("AddonsList");
    const uint64_t programId = m_config.ProgramId();
    if (!list.IsValid() || programId == 0)
    {
        return;
    }

    const uint32_t count = m_romInfo.GetGamePatches(nullptr, 0);
    std::vector<GamePatchInfo> patches(count);
    if (count > 0)
    {
        m_romInfo.GetGamePatches(patches.data(), count);
    }

    const bool emulationRunning = SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
    std::string html;
    html.reserve(patches.size() * 160);
    for (size_t i = 0; i < patches.size(); ++i)
    {
        const GamePatchInfo & patch = patches[i];
        const char * key = patch.key[0] != '\0' ? patch.key : patch.name;
        html += "<div class=\"addons-row\">";
        html += "<div class=\"col-check\"><input type=\"checkbox\" class=\"addon-enabled\"";
        if (patch.enabled)
        {
            html += " checked";
        }
        if (emulationRunning)
        {
            html += " disabled";
        }
        html += stdstr_f(" data-name=\"%s\"", HtmlEscape(key).c_str()).c_str();
        html += " /></div>";
        html += "<div class=\"col-name\">";
        html += HtmlEscape(patch.name);
        html += "</div>";
        html += "<div class=\"col-version\">";
        html += HtmlEscape(patch.version);
        html += "</div>";
        html += "</div>";
    }

    list.SetHTML((const uint8_t *)html.c_str(), html.size());
    AttachClickHandler(m_sciterUI, list, this);
}

void GameConfigAddons::ApplyDisabledAddons(void)
{
    const uint64_t programId = m_config.ProgramId();
    if (!m_page.IsValid() || programId == 0)
    {
        return;
    }

    SciterElement list = m_page.GetElementByID("AddonsList");
    if (!list.IsValid())
    {
        return;
    }

    std::vector<std::string> disabledNames;
    std::vector<const char *> disabledPtrs;
    const uint32_t childCount = list.GetChildCount();
    for (uint32_t i = 0; i < childCount; ++i)
    {
        SciterElement row = list.GetChild(i);
        if (!row.IsValid())
        {
            continue;
        }
        SciterElement checkbox = row.FindFirst("input.addon-enabled");
        if (!checkbox.IsValid())
        {
            continue;
        }
        const bool checked = (checkbox.GetState() & SciterElement::STATE_CHECKED) != 0;
        if (checked)
        {
            continue;
        }
        const std::string name = checkbox.GetAttribute("data-name");
        if (!name.empty())
        {
            disabledNames.push_back(name);
        }
    }

    disabledPtrs.reserve(disabledNames.size());
    for (const std::string & name : disabledNames)
    {
        disabledPtrs.push_back(name.c_str());
    }

    m_modules.Modules().Systemloader().SetDisabledAddons(programId, disabledPtrs.empty() ? nullptr : disabledPtrs.data(), (uint32_t)disabledPtrs.size());
}

void GameConfigAddons::EnforceSingleEnabledUpdate(SciterElement clickedCheckbox)
{
    if (!clickedCheckbox.IsValid())
    {
        return;
    }

    const std::string key = clickedCheckbox.GetAttribute("data-name");
    if (!IsVersionedUpdateKey(key))
    {
        return;
    }

    const bool checked = (clickedCheckbox.GetState() & SciterElement::STATE_CHECKED) != 0;
    if (!checked)
    {
        return;
    }

    SciterElement list = m_page.GetElementByID("AddonsList");
    if (!list.IsValid())
    {
        return;
    }

    const uint32_t childCount = list.GetChildCount();
    for (uint32_t i = 0; i < childCount; ++i)
    {
        SciterElement row = list.GetChild(i);
        if (!row.IsValid())
        {
            continue;
        }
        SciterElement checkbox = row.FindFirst("input.addon-enabled");
        if (!checkbox.IsValid() || checkbox == clickedCheckbox)
        {
            continue;
        }
        const std::string otherKey = checkbox.GetAttribute("data-name");
        if (!IsVersionedUpdateKey(otherKey))
        {
            continue;
        }
        if ((checkbox.GetState() & SciterElement::STATE_CHECKED) != 0)
        {
            checkbox.SetState(0, SciterElement::STATE_CHECKED, true);
        }
    }
}

void GameConfigAddons::RestoreOriginalDisabledAddons(void)
{
    const uint64_t programId = m_config.ProgramId();
    if (programId == 0)
    {
        return;
    }

    std::vector<const char *> disabledPtrs;
    disabledPtrs.reserve(m_originalDisabled.size());
    for (const std::string & name : m_originalDisabled)
    {
        disabledPtrs.push_back(name.c_str());
    }

    m_modules.Modules().Systemloader().SetDisabledAddons(
        programId, disabledPtrs.empty() ? nullptr : disabledPtrs.data(),
        static_cast<uint32_t>(disabledPtrs.size()));
}

void GameConfigAddons::SaveSetting(void)
{
    ApplyDisabledAddons();
}

void GameConfigAddons::RevertSetting(void)
{
    RestoreOriginalDisabledAddons();
}

bool GameConfigAddons::OnClick(SCITER_ELEMENT /*element*/, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement checkbox = FindRowCheckbox(SciterElement(source));
    if (!checkbox.IsValid())
    {
        return false;
    }

    if ((checkbox.GetState() & SciterElement::STATE_DISABLED) != 0)
    {
        return true;
    }

    ToggleCheckbox(checkbox);
    EnforceSingleEnabledUpdate(checkbox);
    ApplyDisabledAddons();
    m_config.RefreshControlMetadata();
    return true;
}
