#include "game_config_addons.h"
#include "game_config.h"
#include "user_interface/html_utils.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>

GameConfigAddons::GameConfigAddons(ISciterUI & sciterUI, GameConfig & config, SystemModules & modules, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_modules(modules),
    m_page(page)
{
    PopulateAddons();
}

void GameConfigAddons::PopulateAddons(void)
{
    SciterElement list = m_page.GetElementByID("AddonsList");
    const uint64_t programId = m_config.ProgramId();
    if (!list.IsValid() || programId == 0)
    {
        return;
    }

    ISystemloader & loader = m_modules.Modules().Systemloader();
    const uint32_t count = loader.GetGamePatches(programId, nullptr, 0);
    std::vector<GamePatchInfo> patches(count);
    if (count > 0)
    {
        loader.GetGamePatches(programId, patches.data(), count);
    }

    const bool emulationRunning = SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
    std::string html;
    html.reserve(patches.size() * 160);
    for (size_t i = 0; i < patches.size(); ++i)
    {
        const GamePatchInfo & patch = patches[i];
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
        html += stdstr_f(" data-name=\"%s\"", HtmlEscape(patch.name).c_str()).c_str();
        html += " /></div>";
        html += "<div class=\"col-name\">";
        html += HtmlEscape(patch.name);
        html += "</div>";
        html += "<div class=\"col-version\">";
        html += HtmlEscape(patch.version);
        html += "</div>";
        html += "</div>";
    }

    list.SetHTML(reinterpret_cast<const uint8_t *>(html.c_str()), html.size());
}
