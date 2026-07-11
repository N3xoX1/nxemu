#include "system_config_hotkeys.h"
#include "settings/ui_identifiers.h"
#include "settings/ui_settings.h"
#include <nxemu-core/notification.h>
#include <nxemu-core/settings/settings.h>
#include <sciter_element.h>

namespace
{
const char * kPressShortcut = "Press shortcut…";

bool ElementIsOrInside(SCITER_ELEMENT descendant, const SciterElement & ancestor)
{
    if (!ancestor.IsValid())
    {
        return false;
    }
    SciterElement el(descendant);
    while (el.IsValid())
    {
        if (el == ancestor)
        {
            return true;
        }
        el = el.GetParent();
    }
    return false;
}

std::string BindingCellText(const MenuBarAccelerator & accel)
{
    std::string s = accel.Format();
    if (s.empty())
    {
        return "(none)";
    }
    return s;
}

struct HotkeyLabelEntry
{
    const char * id;
    const char * label;
};

static const HotkeyLabelEntry kHotkeyLabels[] = {
    {Hotkey::LoadFile, "Load File..."},
    {Hotkey::Exit, "Exit"},
    {Hotkey::Fullscreen, "Fullscreen"},
    {Hotkey::ExitFullscreen, "Exit Fullscreen"},
    {Hotkey::HideUi, "Hide UI"},
    {Hotkey::PauseContinue, "Pause / Continue"},
    {Hotkey::ToggleDockedMode, "Change Docked Mode"},
    {Hotkey::ToggleSpeedLimit, "Toggle Speed Limit"},
    {Hotkey::StopEmulation, "Stop Emulation"},
    {Hotkey::Controllers, "Controllers..."},
    {Hotkey::Configure, "Configure..."},
};

std::string HotkeyBindingElementId(const std::string & hotkeyId)
{
    return "HotkeyBinding-" + hotkeyId;
}
}

SystemConfigHotkeys::SystemConfigHotkeys(ISciterUI & sciterUI, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_window(window),
    m_page(page),
    m_root(window.GetRootElement()),
    m_hotkeysTableBody(page.GetElementByID("HotkeysTableBody")),
    m_captureSinksAttached(false)
{
    DeserializeUIHotkeysMap(SettingsStore::GetInstance().GetString(NXUISetting::Hotkeys), m_sessionHotkeys);
    for (const HotkeyMap::value_type & entry : uiSettings.hotkeys)
    {
        if (m_sessionHotkeys.find(entry.first) == m_sessionHotkeys.end())
        {
            m_sessionHotkeys.insert(entry);
        }
    }
    BuildHotkeysTable();
}

SystemConfigHotkeys::~SystemConfigHotkeys()
{
    if (m_captureSinksAttached && m_root.IsValid())
    {
        m_sciterUI.DetachHandler(m_root, IID_IKEYSINK, (IKeySink *)this);
        m_sciterUI.DetachHandler(m_root, IID_ICLICKSINK, (IClickSink *)this);
        m_captureSinksAttached = false;
    }
    if (m_hotkeysTableBody.IsValid())
    {
        SciterElements cells = m_hotkeysTableBody.FindAll("td.hotkey-binding");
        for (SciterElement & el : cells)
        {
            if (el.IsValid())
            {
                m_sciterUI.DetachHandler(el, IID_IDBLCLICKSINK, (IDoubleClickSink *)this);
            }
        }
    }
}

void SystemConfigHotkeys::BuildHotkeysTable()
{
    if (!m_hotkeysTableBody.IsValid())
    {
        return;
    }
    m_hotkeysTableBody.SetHTML(reinterpret_cast<const uint8_t *>(""), 0, SciterElement::SIH_REPLACE_CONTENT);

    uint32_t rowIndex = 0;
    for (const HotkeyLabelEntry & row : kHotkeyLabels)
    {
        MenuBarAccelerator accel{};
        const HotkeyMap::const_iterator it = m_sessionHotkeys.find(row.id);
        if (it != m_sessionHotkeys.end())
        {
            accel = it->second;
        }
        SciterElement tr;
        if (!tr.Create("tr", ""))
        {
            continue;
        }
        SciterElement tdAction;
        SciterElement tdBind;
        if (!tdAction.Create("td", row.label) || !tdBind.Create("td", BindingCellText(accel).c_str()))
        {
            continue;
        }
        tdAction.AddClassName("hotkey-action");
        tdBind.AddClassName("hotkey-binding");
        const std::string bindId = HotkeyBindingElementId(row.id);
        tdBind.SetAttribute("id", bindId.c_str());
        tdBind.SetAttribute("data-hotkey-id", row.id);
        tr.Insert(tdAction, 0);
        tr.Insert(tdBind, 1);
        m_hotkeysTableBody.Insert(tr, rowIndex);
        rowIndex += 1;
        m_sciterUI.AttachHandler(tdBind, IID_IDBLCLICKSINK, (IDoubleClickSink *)this);
    }
}

bool SystemConfigHotkeys::AcceleratorsEqual(const MenuBarAccelerator & a, const MenuBarAccelerator & b)
{
    return a.key == b.key && a.ctrl == b.ctrl && a.alt == b.alt && a.shift == b.shift;
}

bool SystemConfigHotkeys::ConflictsWithOther(const MenuBarAccelerator & candidate, const std::string & assigningHotkeyId) const
{
    if (candidate.IsNone())
    {
        return false;
    }
    for (HotkeyMap::const_reference p : m_sessionHotkeys)
    {
        if (p.first == assigningHotkeyId)
        {
            continue;
        }
        if (!p.second.IsNone() && AcceleratorsEqual(candidate, p.second))
        {
            return true;
        }
    }
    return false;
}

void SystemConfigHotkeys::UpdateBindingDisplay()
{
    for (HotkeyMap::const_reference p : m_sessionHotkeys)
    {
        SciterElement cell = m_page.GetElementByID(HotkeyBindingElementId(p.first).c_str());
        if (cell.IsValid())
        {
            cell.SetText(BindingCellText(p.second).c_str());
        }
    }
}

void SystemConfigHotkeys::StartCapture(const std::string & hotkeyId)
{
    if (!m_captureHotkeyId.empty())
    {
        EndCapture(true);
    }
    m_captureHotkeyId = hotkeyId;
    SciterElement cell = m_page.GetElementByID(HotkeyBindingElementId(hotkeyId).c_str());
    if (cell.IsValid())
    {
        cell.SetText(kPressShortcut);
    }
    if (m_root.IsValid() && !m_captureSinksAttached)
    {
        m_sciterUI.AttachHandler(m_root, IID_IKEYSINK, (IKeySink *)this);
        m_sciterUI.AttachHandler(m_root, IID_ICLICKSINK, (IClickSink *)this);
        m_captureSinksAttached = true;
    }
}

void SystemConfigHotkeys::EndCapture(bool cancelled)
{
    if (m_captureSinksAttached && m_root.IsValid())
    {
        m_sciterUI.DetachHandler(m_root, IID_IKEYSINK, (IKeySink *)this);
        m_sciterUI.DetachHandler(m_root, IID_ICLICKSINK, (IClickSink *)this);
        m_captureSinksAttached = false;
    }
    m_captureHotkeyId.clear();
    if (cancelled)
    {
        UpdateBindingDisplay();
    }
}

bool SystemConfigHotkeys::OnClick(SCITER_ELEMENT /*element*/, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    if (m_captureHotkeyId.empty())
    {
        return false;
    }
    SciterElement bindingCell(m_page.GetElementByID(HotkeyBindingElementId(m_captureHotkeyId).c_str()));
    if (ElementIsOrInside(source, bindingCell))
    {
        return false;
    }
    EndCapture(true);
    return true;
}

bool SystemConfigHotkeys::OnDoubleClick(SCITER_ELEMENT /*element*/, SCITER_ELEMENT source)
{
    SciterElement src(source);
    std::string hotkeyId = src.GetAttribute("data-hotkey-id");
    if (hotkeyId.empty())
    {
        return false;
    }
    StartCapture(hotkeyId);
    return true;
}

bool SystemConfigHotkeys::OnKeyDown(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*target*/, SciterKeys keyCode, uint32_t keyboardState)
{
    if (m_captureHotkeyId.empty() || !m_captureSinksAttached)
    {
        return false;
    }
    const uint32_t kc = (uint32_t)keyCode;
    if (kc == (uint32_t)SCITER_KEY_DELETE || kc == (uint32_t)SCITER_KEY_BACKSPACE)
    {
        MenuBarAccelerator cleared{};
        m_sessionHotkeys[m_captureHotkeyId] = cleared;
        UpdateBindingDisplay();
        EndCapture(false);
        return true;
    }
    MenuBarAccelerator built = MenuBarAcceleratorFromKeyEvent(kc, keyboardState);
    if (built.IsNone())
    {
        return true;
    }
    const std::string assigningId = m_captureHotkeyId;
    if (ConflictsWithOther(built, assigningId))
    {
        g_notify->DisplayError("This shortcut is already assigned to another action.", "NxEmu");
        EndCapture(true);
        return true;
    }
    m_sessionHotkeys[assigningId] = built;
    UpdateBindingDisplay();
    EndCapture(false);
    return true;
}

bool SystemConfigHotkeys::OnKeyUp(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*target*/, SciterKeys /*keyCode*/, uint32_t /*keyboardState*/)
{
    if (!m_captureHotkeyId.empty())
    {
        return true;
    }
    return false;
}

bool SystemConfigHotkeys::OnKeyChar(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*target*/, SciterKeys /*keyCode*/, uint32_t /*keyboardState*/)
{
    return !m_captureHotkeyId.empty();
}

void SystemConfigHotkeys::SaveSetting()
{
    SettingsStore & store = SettingsStore::GetInstance();
    store.SetString(NXUISetting::Hotkeys, SerializeUIHotkeysMap(m_sessionHotkeys).c_str());
}
