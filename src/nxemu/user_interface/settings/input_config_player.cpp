#include "input_config.h"
#include "input_config_player.h"
#include <common/std_string.h>
#include <sciter_handler.h>
#include <widgets/combo_box.h>
#include <unordered_map>
#include <numbers>
#include <stdexcept>
#include "user_interface/key_mappings.h"
#include <yuzu_common/string_util.h>
#include <yuzu_common/vector_math.h>
#include "settings/ui_settings.h"
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>

namespace
{
    enum VirtualKeyCodes
    {
        VK_SHIFT = 0x10,
        VK_LEFT = 0x25,
        VK_UP = 0x26,
        VK_RIGHT = 0x27,
        VK_DOWN = 0x28,
    };

    static constexpr int ANALOG_SUB_BUTTONS_NUM = 4;

    constexpr char KEY_VALUE_SEPARATOR = ':';
    constexpr char PARAM_SEPARATOR = ',';

    constexpr char ESCAPE_CHARACTER = '$';
    constexpr char KEY_VALUE_SEPARATOR_ESCAPE[] = "$0";
    constexpr char PARAM_SEPARATOR_ESCAPE[] = "$1";
    constexpr char ESCAPE_CHARACTER_ESCAPE[] = "$2";

    constexpr char EMPTY_PLACEHOLDER[] = "[empty]";
    using DataType = std::unordered_map<std::string, std::string>;

    class ButtonMappingListPtr
    {
    public:
        explicit ButtonMappingListPtr(IButtonMappingList* ptr = nullptr) : 
            m_ptr(ptr) 
        {
        }

        ~ButtonMappingListPtr()
        {
            if (m_ptr)
            {
                m_ptr->Release();
            }
        }

        ButtonMappingListPtr(ButtonMappingListPtr&& other) noexcept : 
            m_ptr(other.m_ptr)
        {
            other.m_ptr = nullptr;
        }

        ButtonMappingListPtr& operator=(ButtonMappingListPtr&& other) noexcept
        {
            if (this != &other)
            {
                if (m_ptr)
                {
                    m_ptr->Release();
                }
                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        IButtonMappingList* operator->() const { return m_ptr; }
        IButtonMappingList& operator*() const { return *m_ptr; }
        IButtonMappingList* get() const { return m_ptr; }
        explicit operator bool() const { return m_ptr != nullptr; }

    private:
        ButtonMappingListPtr(const ButtonMappingListPtr&) = delete;
        ButtonMappingListPtr& operator=(const ButtonMappingListPtr&) = delete;

        IButtonMappingList* m_ptr;
    };

    class ParamPackage :
        public IParamPackage
    {
    public:
        ParamPackage() :
            m_package(nullptr)
        {
        }

        ParamPackage(const std::string & serialized) :
            m_package(nullptr)
        {
            if (serialized == EMPTY_PLACEHOLDER)
            {
                return;
            }
            std::vector<std::string> pairs;
            Common::SplitString(serialized, PARAM_SEPARATOR, pairs);

            for (const std::string& pair : pairs)
            {
                std::vector<std::string> key_value;
                Common::SplitString(pair, KEY_VALUE_SEPARATOR, key_value);
                if (key_value.size() != 2) 
                {
                    continue;
                }

                for (std::string& part : key_value) {
                    part = Common::ReplaceAll(part, KEY_VALUE_SEPARATOR_ESCAPE, { KEY_VALUE_SEPARATOR });
                    part = Common::ReplaceAll(part, PARAM_SEPARATOR_ESCAPE, { PARAM_SEPARATOR });
                    part = Common::ReplaceAll(part, ESCAPE_CHARACTER_ESCAPE, { ESCAPE_CHARACTER });
                }

                Set(key_value[0], std::move(key_value[1]));
            }
        }

        ParamPackage(std::initializer_list<DataType::value_type> list) : 
            data(list) ,
            m_package(nullptr)
        {
        }

        ParamPackage(ParamPackage&& other) noexcept:
            data(std::move(other.data)),
            m_package(std::exchange(other.m_package, nullptr))
        {
        }

        ParamPackage(IParamPackage * package) :
            m_package(package)
        {
        }

        ~ParamPackage()
        {
            if (m_package)
            {
                m_package->Release();
                m_package = nullptr;
            }
        }

        ParamPackage& operator=(const ParamPackage& other)
        {
            if (this != &other)
            {
                if (m_package)
                {
                    m_package->Release();
                    m_package = nullptr;
                }

                m_package = other.m_package;
                data = std::move(other.data);
                m_serializeData = m_serializeData;
            }
            return *this;
        }

        ParamPackage& operator=(ParamPackage && other)
        {
            if (this != &other)
            {
                if (m_package)
                {
                    m_package->Release();
                    m_package = nullptr;
                }

                m_package = other.m_package;
                data = std::move(other.data);
                m_serializeData = std::move(other.m_serializeData);
                other.m_package = nullptr;
            }
            return *this;
        }

        operator IParamPackage & () const
        {
            if (m_package == nullptr)
            {
                return *((IParamPackage *)this);
            }
            return *m_package;
        }

        IParamPackage * operator->() const
        {
            if (m_package == nullptr)
            {
                return (IParamPackage*)this;
            }
            return m_package;
        }
    

        bool Has(const char * key) const override
        {
            if (m_package != nullptr)
            {
                return m_package->Has(key);
            }
            return data.find(key) != data.end();
        }

        bool GetBool(const char * key, bool default_value) const override
        {
            if (m_package != nullptr)
            {
                return m_package->GetBool(key, default_value);
            }
            DataType::const_iterator pair = data.find(key);
            if (pair == data.end()) 
            {
                return default_value;
            }

            try 
            {
                return std::stoi(pair->second) != 0;
            }
            catch (const std::logic_error&) 
            {
                return default_value;
            }
        }

        int32_t GetInt(const char * key, int32_t default_value) const override
        {
            if (m_package != nullptr)
            {
                return m_package->GetInt(key, default_value);
            }
            DataType::const_iterator pair = data.find(key);
            if (pair == data.end())
            {
                return default_value;
            }

            try
            {
                return std::stoi(pair->second);
            }
            catch (const std::logic_error&)
            {
                return default_value;
            }
        }

        float GetFloat(const char * key, float default_value) const override
        {
            if (m_package != nullptr)
            {
                return m_package->GetFloat(key, default_value);
            }
            __debugbreak();
            return 0.0f;
        }

        const char * GetString(const char * key, const char * default_value) const override
        {
            if (m_package != nullptr)
            {
                return m_package->GetString(key, default_value);
            }
            DataType::const_iterator pair = data.find(key);
            if (pair == data.end()) 
            {
                return default_value;
            }
            return pair->second.c_str();
        }

        void SetFloat(const char * key, float value) override
        {
            if (m_package != nullptr)
            {
                m_package->SetFloat(key, value);
            }
            else
            {
                data.insert_or_assign(key, std::to_string(value));
            }
        }

        const char * Serialize() const override
        {
            if (m_package != nullptr)
            {
                return m_package->Serialize();
            }

            if (data.empty())
            {
                return EMPTY_PLACEHOLDER;
            }

            std::string result;

            for (const DataType::value_type & pair : data) {
                std::array<std::string, 2> key_value{ {pair.first, pair.second} };
                for (std::string& part : key_value) {
                    part = Common::ReplaceAll(part, { ESCAPE_CHARACTER }, ESCAPE_CHARACTER_ESCAPE);
                    part = Common::ReplaceAll(part, { PARAM_SEPARATOR }, PARAM_SEPARATOR_ESCAPE);
                    part = Common::ReplaceAll(part, { KEY_VALUE_SEPARATOR }, KEY_VALUE_SEPARATOR_ESCAPE);
                }
                result += key_value[0] + KEY_VALUE_SEPARATOR + key_value[1] + PARAM_SEPARATOR;
            }

            result.pop_back();
            m_serializeData = result;
            return m_serializeData.c_str();
        }

        void Release()
        {
        }

        void Set(const std::string& key, std::string value) {
            data.insert_or_assign(key, std::move(value));
        }

        void Set(const std::string& key, int value) {
            data.insert_or_assign(key, std::to_string(value));
        }

        void Set(const std::string& key, float value) {
            data.insert_or_assign(key, std::to_string(value));
        }

    private:
        ParamPackage(const ParamPackage&) = delete;

        IParamPackage * m_package;
        DataType data;
        mutable std::string m_serializeData;
    };

    std::string GetKeyName(int key_code) 
    {
        enum Key : int
        {
            Shift = 0x01000020,
            Control = 0x01000021,
            Alt = 0x01000023,
            Meta = 0x01000022,
        };
        
        switch (key_code) {
        case Key::Shift: return "Shift";
        case Key::Control: return "Ctrl";
        case Key::Alt: return "Alt";
        case Key::Meta: return {};
        }
        return KeyCodeToString(key_code);
    }

    std::string GenerateKeyboardParam(int key_code) 
    {
        ParamPackage param;
        param.Set("engine", "keyboard");
        param.Set("code", key_code);
        param.Set("toggle", false);
        return param.Serialize();
    }

    void SetAnalogParam(const ParamPackage & input_param, ParamPackage & analog_param, const std::string& button_name) 
    {
        // The poller returned a complete axis, so set all the buttons
        if (input_param.Has("axis_x") && input_param.Has("axis_y"))
        {
            analog_param = input_param;
            return;
        }
        // Check if the current configuration has either no engine or an axis binding.
        // Clears out the old binding and adds one with analog_from_button.
        if (!analog_param.Has("engine") || analog_param.Has("axis_x") || analog_param.Has("axis_y"))
        {
            analog_param = {
                {"engine", "analog_from_button"},
            };
        }
        analog_param.Set(button_name, input_param.Serialize());
    }

    const std::array<std::string, ANALOG_SUB_BUTTONS_NUM> analog_sub_buttons{ { "up", "down", "left", "right", } };

    const std::array<int, (uint32_t)NativeButtonValues::NumButtons> default_buttons = {
        'C', 'X', 'V', 'Z', 'F',
        'G', 'Q', 'E', 'R', 'T',
        'M', 'N', VK_LEFT, VK_UP, VK_RIGHT,
        VK_DOWN, 'Q', 'E', 0, 0,
        'Q', 'E',
    };

    const std::array<std::array<int, 4>, (size_t)NativeAnalogValues::NumAnalogs> default_analogs{ {
    {
        'W','S','A','D',
    },
    {
        'I','K','J','L',
    },}};
    
    const std::array<int, (uint32_t)NativeMotionValues::NumMotions>default_motions = { '7', '8', };

    const std::array<int, 2> default_stick_mod = { VK_SHIFT, 0, };

    std::string points_string(const std::array<std::pair<float, float>, 4>& v) 
    {
        return stdstr_f("%f,%f %f,%f %f,%f %f,%f", v[0].first, v[0].second, v[1].first, v[1].second, v[2].first, v[2].second, v[3].first, v[3].second);
    }

    void PaintMotionCubeGroup(SciterElement motion, const vec3f_t& eulerValue, float size)
    {
        if (!motion.IsValid())
        {
            return;
        }
        const Common::Vec3f euler(eulerValue.x, eulerValue.y, eulerValue.z);
        std::array<Common::Vec3f, 8> cube {{
            {-0.7f, -1, -0.5f}, {-0.7f, 1, -0.5f}, {0.7f, 1, -0.5f}, {0.7f, -1, -0.5f},
            {-0.7f, -1, 0.5f}, {-0.7f, 1, 0.5f}, {0.7f, 1, 0.5f}, {0.7f, -1, 0.5f},
        }};
        for (Common::Vec3f & point : cube)
        {
            point.RotateFromOrigin(euler.x, euler.y, euler.z);
            point *= size;
        }
        const std::array<std::pair<float, float>, 4> front = {{
            {cube[0].x, cube[0].y}, {cube[1].x, cube[1].y},
            {cube[2].x, cube[2].y}, {cube[3].x, cube[3].y}
        }};
        const std::array<std::pair<float, float>, 4> back = {{
            {cube[4].x, cube[4].y}, {cube[5].x, cube[5].y},
            {cube[6].x, cube[6].y}, {cube[7].x, cube[7].y}
        }};
        SciterElement poly_front = motion.FindFirst(".cube.face.front");
        if (poly_front.IsValid())
        {
            poly_front.SetAttribute("points", points_string(front).c_str());
        }
        SciterElement poly_back = motion.FindFirst(".cube.face.back");
        if (poly_back.IsValid())
        {
            poly_back.SetAttribute("points", points_string(back).c_str());
        }
        const int A[4] = { 0, 1, 2, 3 };
        const int B[4] = { 4, 5, 6, 7 };
        for (int i = 0; i < 4; ++i)
        {
            const std::string sel = std::string(".cube.edge.e") + char('0' + i);
            SciterElement e = motion.FindFirst(sel.c_str());
            if (!e.IsValid())
            {
                continue;
            }
            e.SetAttribute("x1", stdstr_f("%f", cube[A[i]].x).c_str());
            e.SetAttribute("y1", stdstr_f("%f", cube[A[i]].y).c_str());
            e.SetAttribute("x2", stdstr_f("%f", cube[B[i]].x).c_str());
            e.SetAttribute("y2", stdstr_f("%f", cube[B[i]].y).c_str());
        }
    }

    std::string GetButtonName(ButtonNames button_name)
    {
        switch (button_name)
        {
        case ButtonNames::ButtonLeft: return "Left";
        case ButtonNames::ButtonRight: return "Right";
        case ButtonNames::ButtonDown: return "Down";
        case ButtonNames::ButtonUp: return "Up";
        case ButtonNames::TriggerZ: return "Z";
        case ButtonNames::TriggerR: return "R";
        case ButtonNames::TriggerL: return "L";
        case ButtonNames::TriggerZR: return "ZR";
        case ButtonNames::TriggerZL: return "ZL";
        case ButtonNames::TriggerSR: return "SR";
        case ButtonNames::TriggerSL: return "SL";
        case ButtonNames::ButtonStickL: return "Stick L";
        case ButtonNames::ButtonStickR: return "Stick R";
        case ButtonNames::ButtonA: return "A";
        case ButtonNames::ButtonB: return "B";
        case ButtonNames::ButtonX: return "X";
        case ButtonNames::ButtonY: return "Y";
        case ButtonNames::ButtonStart: return "Start";
        case ButtonNames::ButtonPlus: return "Plus";
        case ButtonNames::ButtonMinus: return "Minus";
        case ButtonNames::ButtonHome: return "Home";
        case ButtonNames::ButtonCapture: return "Capture";
        case ButtonNames::L1: return "L1";
        case ButtonNames::L2: return "L2";
        case ButtonNames::L3: return "L3";
        case ButtonNames::R1: return "R1";
        case ButtonNames::R2: return "R2";
        case ButtonNames::R3: return "R3";
        case ButtonNames::Circle: return "Circle";
        case ButtonNames::Cross: return "Cross";
        case ButtonNames::Square: return "Square";
        case ButtonNames::Triangle: return "Triangle";
        case ButtonNames::Share: return "Share";
        case ButtonNames::Options: return "Options";
        case ButtonNames::Home: return "Home";
        case ButtonNames::Touch: return "Touch";
        case ButtonNames::ButtonMouseWheel: return "Wheel";
        case ButtonNames::ButtonBackward: return "Backward";
        case ButtonNames::ButtonForward: return "Forward";
        case ButtonNames::ButtonTask: return "Task";
        case ButtonNames::ButtonExtra: return "Extra";
        }
        return "[undefined]";
    }

    const char * GetControllerSvgResource(NpadStyleIndex layout)
    {
        switch (layout)
        {
        case NpadStyleIndex::Fullkey:
            return "pro_controller.svg";
        case NpadStyleIndex::Handheld:
            return "handheld.svg";
        case NpadStyleIndex::JoyconDual:
            return "dual_joycons.svg";
        case NpadStyleIndex::JoyconLeft:
            return "left_joycon.svg";
        case NpadStyleIndex::JoyconRight:
            return "right_joycon.svg";
        case NpadStyleIndex::GameCube:
            return "gc_controller.svg";
        default:
            return nullptr;
        }
    }

    const std::string & LoadCachedControllerSvg(ISciterUI & sciterUI, const char * resource)
    {
        static std::unordered_map<std::string, std::string> cache;
        const auto found = cache.find(resource);
        if (found != cache.end())
        {
            return found->second;
        }
        std::vector<uint8_t> data;
        if (!sciterUI.LoadResource(resource, data) || data.empty())
        {
            static const std::string empty;
            return empty;
        }
        return cache.emplace(resource, std::string(reinterpret_cast<const char *>(data.data()), data.size())).first->second;
    }
} // namespace

InputConfigPlayer::InputConfigPlayer(ISciterUI & sciterUI, InputConfig & config, SystemModules & modules, HWINDOW parent, SciterElement page, NpadIdType controllerIndex) :
    m_operatingSystem(modules.Modules().OperatingSystem()),
    m_sciterUI(sciterUI),
    m_config(config),
    m_modules(modules),
    m_parent(parent),
    m_page(page),
    m_controllerIndex(controllerIndex),
    m_emulatedController(nullptr),
    m_emulatedControllerPlayer(modules.Modules().OperatingSystem().GetEmulatedController(controllerIndex)),
    m_emulatedControllerHandheld(modules.Modules().OperatingSystem().GetEmulatedController(NpadIdType::Handheld)),
    m_inputDeviceList(config.InputDeviceList()),
    m_timeoutTimerActive(false),
    m_pollingType(PollingInputType::None),
    m_pollingButtonId(0),
    m_stickUiThrottleLast{},
    m_displayedControllerLayout(NpadStyleIndex::None),
    m_callbackEnabled(true)
{
    memset(&m_buttonValues, 0, sizeof(m_buttonValues));
    m_emulatedController = &m_emulatedControllerPlayer;
    m_emulatedController->SetControllerEventCallback(stControllerEventCallback,this);
    BindControls();
    LoadConfiguration();
    UpdateControllerAvailableButtons();
    UpdateControllerEnabledButtons();
    UpdateControllerButtonNames();
    UpdateMotionButtons();
    SettingsStore::GetInstance().RegisterCallback(NXOsSetting::VibrationEnabled, stVibrationSettingChanged, this);
}

InputConfigPlayer::~InputConfigPlayer()
{
    m_callbackEnabled = false;
    SettingsStore::GetInstance().UnregisterCallback(NXOsSetting::VibrationEnabled, stVibrationSettingChanged, this);
    UnbindControls();
    if (m_emulatedController != nullptr)
    {
        m_emulatedController->SetControllerEventCallback(nullptr, nullptr);
    }
    if (m_pollingType != PollingInputType::None)
    {
        m_operatingSystem.StopMapping();
    }
    if (m_page.IsValid())
    {
        m_page.SetTimer(0, (uint32_t *)TIMER_TIMEOUT);
        m_page.SetTimer(0, (uint32_t *)TIMER_POLL);
    }
}

void InputConfigPlayer::SaveSetting()
{
    m_emulatedController->SaveCurrentConfig();
    SaveVibrationControls();
}

bool InputConfigPlayer::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    if (element == m_connectedController)
    {
        bool connected = (m_connectedController.GetState() & SciterElement::STATE_CHECKED) != 0;
        if (connected)
        {
            m_emulatedController->Connect(true);
        }
        else 
        {
            m_emulatedController->Disconnect();
        }
    }
    else if (element == m_vibrationStrengthPopupBtn || source == m_vibrationStrengthPopupBtn ||
             element == m_vibrationStrengthLabel || source == m_vibrationStrengthLabel ||
             element == m_vibrationStrengthAnchor || source == m_vibrationStrengthAnchor)
    {
        if (!SettingsStore::GetInstance().GetBool(NXOsSetting::VibrationEnabled))
        {
            return true;
        }
        if (m_vibrationStrengthPopup.IsValid() && m_vibrationStrengthAnchor.IsValid())
        {
            const bool popupOpen = (m_vibrationStrengthPopup.GetState() & SciterElement::STATE_POPUP) != 0;
            if (!popupOpen)
            {
                m_sciterUI.PopupShow(m_vibrationStrengthPopup, m_vibrationStrengthAnchor, 8);
                if (m_vibrationStrengthPopupBtn.IsValid())
                {
                    m_vibrationStrengthPopupBtn.AddClassName("open");
                }
            }
            else
            {
                m_sciterUI.PopupHide(m_vibrationStrengthPopup);
                if (m_vibrationStrengthPopupBtn.IsValid())
                {
                    m_vibrationStrengthPopupBtn.RemoveClassName("open");
                }
            }
        }
        return true;
    }
    else
    {
        for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
        {
            if (!m_buttonMap[i].IsValid() || element != m_buttonMap[i])
            {
                continue;
            }
            HandleClick(m_buttonMap[i], i, PollingInputType::Button);
            return false;
        }
        for (uint32_t i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
        {
            if (!m_motionMap[i].IsValid() || element != m_motionMap[i])
            {
                continue;
            }
            HandleClick(m_motionMap[i], i, PollingInputType::Motion);
            return false;
        }
    }
    return false;
}

bool InputConfigPlayer::OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t event_code, uint64_t /*reason*/)
{
    if (event_code == static_cast<uint32_t>(SciterBehaviorEvent::PopupDismissed) &&
        m_vibrationStrengthPopup.IsValid())
    {
        for (SciterElement walk(element); walk.IsValid(); walk = walk.GetParent())
        {
            if (walk == m_vibrationStrengthPopup)
            {
                if (m_vibrationStrengthPopupBtn.IsValid())
                {
                    m_vibrationStrengthPopupBtn.RemoveClassName("open");
                }
                break;
            }
        }
    }
    return false;
}

bool InputConfigPlayer::OnStateChange(SCITER_ELEMENT elem, uint32_t /*eventReason*/, void * /*data*/)
{
    if (m_page.GetElementByID("comboControllerType") == elem)
    {
        ControllerTypeChanged();
    }
    else if (m_page.GetElementByID("comboDevices") == elem)
    {
        UpdateMappingWithDefaults();
    }
    else if (m_analogMapDeadzoneSlider[0] == elem)
    {
        DeadzoneSliderChanged(0);
    }
    else if (m_analogMapDeadzoneSlider[1] == elem)
    {
        DeadzoneSliderChanged(1);
    }
    else if (m_vibrationStrength == elem)
    {
        UpdateVibrationStrengthDisplay();
    }
    return false;
}

bool InputConfigPlayer::OnTimer(SCITER_ELEMENT /*element*/, uint32_t * timerId)
{
    if (timerId == (uint32_t*)TIMER_POLL)
    {
        constexpr int kMaxDrainPerPoll = 64;
        int drain = 0;
        for (; drain < kMaxDrainPerPoll; ++drain)
        {
            ParamPackage param(m_operatingSystem.GetNextInput());
            if (!param.Has("engine"))
            {
                break;
            }
            if (!IsInputAcceptable(param))
            {
                continue;
            }
            if (m_pollingType == PollingInputType::Button)
            {
                m_emulatedController->SetButtonParam(m_pollingButtonId, param);
            }
            else if (m_pollingType == PollingInputType::Motion)
            {
                m_emulatedController->SetMotionParam(m_pollingButtonId, param);
            }
            else
            {
                continue;
            }
            PollingDone();
            return false;
        }
    }
    else if (timerId == (uint32_t*)TIMER_TIMEOUT)
    {
        PollingDone();
        return false;
    }
    return true;
}

bool InputConfigPlayer::OnKeyDown(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys keyCode, uint32_t /*keyboardState*/)
{
    if (m_pollingType == PollingInputType::None)
    {
        return false;
    }
    if (keyCode != SCITER_KEY_ESCAPE)
    {
        int keyIndex = SciterKeyToSwitchKey(keyCode);
        if (keyIndex != 0)
        {
            m_operatingSystem.KeyboardKeyPress(0, keyIndex, SciterKeyToVKCode(keyCode));
        }
    }
    return false;
}

bool InputConfigPlayer::OnKeyUp(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys keyCode, uint32_t /*keyboardState*/)
{
    if (m_pollingType == PollingInputType::None)
    {
        return false;
    }
    if (keyCode != SCITER_KEY_ESCAPE)
    {
        int keyIndex = SciterKeyToSwitchKey(keyCode);
        if (keyIndex != 0)
        {
            m_operatingSystem.KeyboardKeyRelease(0, keyIndex, SciterKeyToVKCode(keyCode));
        }
    }
    return false;
}

bool InputConfigPlayer::OnKeyChar(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*item*/, SciterKeys /*keyCode*/, uint32_t /*keyboardState*/)
{
    return false;
}

void InputConfigPlayer::BindControls()
{
    m_sciterUI.AttachHandler(m_page, IID_ITIMERSINK, (ITimerSink*)this);

    m_buttonMap[0] = m_page.GetElementByID("ButtonA");
    m_buttonMap[1] = m_page.GetElementByID("ButtonB");
    m_buttonMap[2] = m_page.GetElementByID("ButtonX");
    m_buttonMap[3] = m_page.GetElementByID("ButtonY");
    m_buttonMap[4] = m_page.GetElementByID("ButtonLStick");
    m_buttonMap[5] = m_page.GetElementByID("ButtonRStick");
    m_buttonMap[6] = m_page.GetElementByID("ButtonL");
    m_buttonMap[7] = m_page.GetElementByID("ButtonR");
    m_buttonMap[8] = m_page.GetElementByID("ButtonZL");
    m_buttonMap[9] = m_page.GetElementByID("ButtonZR");
    m_buttonMap[10] = m_page.GetElementByID("ButtonPlus");
    m_buttonMap[11] = m_page.GetElementByID("ButtonMinus");
    m_buttonMap[12] = m_page.GetElementByID("ButtonDpadLeft");
    m_buttonMap[13] = m_page.GetElementByID("ButtonDpadUp");
    m_buttonMap[14] = m_page.GetElementByID("ButtonDpadRight");
    m_buttonMap[15] = m_page.GetElementByID("ButtonDpadDown");
    m_buttonMap[16] = m_page.GetElementByID("ButtonSLLeft");
    m_buttonMap[17] = m_page.GetElementByID("ButtonSRLeft");
    m_buttonMap[18] = m_page.GetElementByID("ButtonHome");
    m_buttonMap[19] = m_page.GetElementByID("ButtonScreenshot");
    m_buttonMap[20] = m_page.GetElementByID("ButtonSLRight");
    m_buttonMap[21] = m_page.GetElementByID("ButtonSRRight");

    m_motionMap[0] = m_page.GetElementByID("buttonMotionLeft");
    m_motionMap[1] = m_page.GetElementByID("buttonMotionRight");
    m_motionGroup[0] = m_page.GetElementByID("buttonMotionLeftGroup");
    m_motionGroup[1] = m_page.GetElementByID("buttonMotionRightGroup");
    m_centerButtonsMotion = m_page.GetElementByID("centerButtonsMotion");

    m_analogMapButtons[0][0] = m_page.GetElementByID("ButtonLStickUp");
    m_analogMapButtons[0][1] = m_page.GetElementByID("ButtonLStickDown");
    m_analogMapButtons[0][2] = m_page.GetElementByID("ButtonLStickLeft");
    m_analogMapButtons[0][3] = m_page.GetElementByID("ButtonLStickRight");

    m_analogMapButtons[1][0] = m_page.GetElementByID("ButtonRStickUp");
    m_analogMapButtons[1][1] = m_page.GetElementByID("ButtonRStickDown");
    m_analogMapButtons[1][2] = m_page.GetElementByID("ButtonRStickLeft");
    m_analogMapButtons[1][3] = m_page.GetElementByID("ButtonRStickRight");

    m_analogMapModifierButton[0] = m_page.GetElementByID("ButtonLStickMod");
    m_analogMapModifierButton[1] = m_page.GetElementByID("ButtonRStickMod");

    m_analogMapDeadzoneLabel[0] = m_page.GetElementByID("labelLStickDeadzone");
    m_analogMapDeadzoneLabel[1] = m_page.GetElementByID("labelRStickDeadzone");
    m_analogMapDeadzoneSlider[0] = m_page.GetElementByID("sliderLStickDeadzone");
    m_analogMapDeadzoneSlider[1] = m_page.GetElementByID("sliderRStickDeadzone");
    m_analogMapModifierGroupbox[0] = m_page.GetElementByID("buttonLStickModGroup");
    m_analogMapModifierGroupbox[1] = m_page.GetElementByID("buttonRStickModGroup");
    m_analogMapModifierButton[0] = m_page.GetElementByID("buttonLStickMod");
    m_analogMapModifierButton[1] = m_page.GetElementByID("buttonRStickMod");
    m_analogMapModifierLabel[0] = m_page.GetElementByID("labelLStickModifierRange");
    m_analogMapModifierLabel[1] = m_page.GetElementByID("labelRStickModifierRange");
    m_analogMapModifierSlider[0] = m_page.GetElementByID("sliderLStickModifierRange");
    m_analogMapModifierSlider[1] = m_page.GetElementByID("sliderRStickModifierRange");
    m_analogMapRangeGroupbox[0] = m_page.GetElementByID("buttonLStickRangeGroup");
    m_analogMapRangeGroupbox[1] = m_page.GetElementByID("buttonRStickRangeGroup");
    m_analogMapRangeSpinbox[0] = m_page.GetElementByID("spinboxLStickRange");
    m_analogMapRangeSpinbox[1] = m_page.GetElementByID("spinboxRStickRange");

    for (uint32_t i = 0; i < (uint32_t)NativeAnalogValues::NumAnalogs; i++)
    {
        if (m_analogMapDeadzoneSlider[i].IsValid())
        {
            m_sciterUI.AttachHandler(m_analogMapDeadzoneSlider[i], IID_ISTATECHANGESINK, (IStateChangeSink*)this);
        }
    }

    for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
    {
        if (!m_buttonMap[i].IsValid())
        {
            continue;
        }
        m_sciterUI.AttachHandler(m_buttonMap[i], IID_ICLICKSINK, (IClickSink*)this);
    }

    for (uint32_t i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
    {
        if (!m_motionMap[i].IsValid())
        {
            continue;
        }
        m_sciterUI.AttachHandler(m_motionMap[i], IID_ICLICKSINK, (IClickSink*)this);
    }

    SciterElement comboControllerType = m_page.GetElementByID("comboControllerType");
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(comboControllerType, IID_ICOMBOBOX);
    if (interfacePtr)
    {
        m_comboControllerType = std::static_pointer_cast<IComboBox>(interfacePtr);
    }
    m_sciterUI.AttachHandler(comboControllerType, IID_ISTATECHANGESINK, (IStateChangeSink*)this);

    SciterElement comboDevices = m_page.GetElementByID("comboDevices");
    interfacePtr = m_sciterUI.GetElementInterface(comboDevices, IID_ICOMBOBOX);
    if (interfacePtr)
    {
        m_comboDevices = std::static_pointer_cast<IComboBox>(interfacePtr);
    }
    m_sciterUI.AttachHandler(comboDevices, IID_ISTATECHANGESINK, (IStateChangeSink*)this);

    SciterElement ConnectedController = m_page.GetElementByID("ConnectedController");
    if (ConnectedController)
    {
        m_sciterUI.AttachHandler(ConnectedController, IID_ICLICKSINK, (IClickSink*)this);
    }

    m_vibrationGroup = m_page.FindFirst(".group.vibration");
    m_vibrationEnabled = m_page.GetElementByID("VibrationEnabled");
    m_vibrationStrength = m_page.GetElementByID("sliderVibrationStrength");
    m_vibrationStrengthLabel = m_page.GetElementByID("labelVibrationStrength");
    m_vibrationStrengthAnchor = m_page.GetElementByID("vibrationStrengthAnchor");
    m_vibrationStrengthPopupBtn = m_page.GetElementByID("vibrationStrengthPopupBtn");
    m_vibrationStrengthPopup = m_page.GetElementByID("VibrationStrengthPopup");
    if (m_vibrationStrength.IsValid())
    {
        m_sciterUI.AttachHandler(m_vibrationStrength, IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    }
    if (m_vibrationStrengthLabel.IsValid())
    {
        m_sciterUI.AttachHandler(m_vibrationStrengthLabel, IID_ICLICKSINK, (IClickSink*)this);
    }
    if (m_vibrationStrengthPopupBtn.IsValid())
    {
        m_sciterUI.AttachHandler(m_vibrationStrengthPopupBtn, IID_ICLICKSINK, (IClickSink*)this);
    }
    if (m_vibrationStrengthPopup.IsValid())
    {
        m_sciterUI.AttachHandler(m_vibrationStrengthPopup, IID_EVENTSINK, (IEventSink*)this);
    }

    SetConnectableControllers();
}

void InputConfigPlayer::UnbindControls()
{
    if (m_vibrationStrengthPopup.IsValid())
    {
        m_sciterUI.PopupHide(m_vibrationStrengthPopup);
        m_sciterUI.DetachHandler(m_vibrationStrengthPopup, IID_EVENTSINK, (IEventSink*)this);
    }
    if (m_vibrationStrengthPopupBtn.IsValid())
    {
        m_sciterUI.DetachHandler(m_vibrationStrengthPopupBtn, IID_ICLICKSINK, (IClickSink*)this);
    }
    if (m_vibrationStrengthLabel.IsValid())
    {
        m_sciterUI.DetachHandler(m_vibrationStrengthLabel, IID_ICLICKSINK, (IClickSink*)this);
    }
    if (m_vibrationStrength.IsValid())
    {
        m_sciterUI.DetachHandler(m_vibrationStrength, IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    }

    SciterElement ConnectedController = m_page.GetElementByID("ConnectedController");
    if (ConnectedController.IsValid())
    {
        m_sciterUI.DetachHandler(ConnectedController, IID_ICLICKSINK, (IClickSink*)this);
    }

    SciterElement comboDevices = m_page.GetElementByID("comboDevices");
    if (comboDevices.IsValid())
    {
        m_sciterUI.DetachHandler(comboDevices, IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    }

    SciterElement comboControllerType = m_page.GetElementByID("comboControllerType");
    if (comboControllerType.IsValid())
    {
        m_sciterUI.DetachHandler(comboControllerType, IID_ISTATECHANGESINK, (IStateChangeSink*)this);
    }

    for (uint32_t i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
    {
        if (m_motionMap[i].IsValid())
        {
            m_sciterUI.DetachHandler(m_motionMap[i], IID_ICLICKSINK, (IClickSink*)this);
        }
    }

    for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
    {
        if (m_buttonMap[i].IsValid())
        {
            m_sciterUI.DetachHandler(m_buttonMap[i], IID_ICLICKSINK, (IClickSink*)this);
        }
    }

    for (uint32_t i = 0; i < (uint32_t)NativeAnalogValues::NumAnalogs; i++)
    {
        if (m_analogMapDeadzoneSlider[i].IsValid())
        {
            m_sciterUI.DetachHandler(m_analogMapDeadzoneSlider[i], IID_ISTATECHANGESINK, (IStateChangeSink*)this);
        }
    }

    if (m_page.IsValid())
    {
        m_sciterUI.DetachHandler(m_page, IID_ITIMERSINK, (ITimerSink*)this);
    }
}

void InputConfigPlayer::LoadConfiguration()
{
    m_emulatedController->ReloadFromSettings();
    UpdateUI();
    UpdateInputDeviceCombobox();
    LoadVibrationControls();

    if (m_comboControllerType != nullptr)
    {
        const int comboBoxIndex = GetIndexFromControllerType(m_emulatedController->GetNpadStyleIndex(true));
        m_comboControllerType->SelectItem(comboBoxIndex);
    }

    m_connectedController = m_page.GetElementByID("ConnectedController");
    if (m_connectedController)
    {
        bool checked = m_emulatedController->IsConnected(true);
        m_connectedController.SetState(checked ? SciterElement::STATE_CHECKED : 0, checked ? 0 : SciterElement::STATE_CHECKED, true);
    }
}

void InputConfigPlayer::UpdateInputDeviceCombobox(void)
{
    if (m_comboDevices == nullptr)
    {
        return;
    }

    // Skip input device persistence if "Input Devices" is set to "Any".
    if (m_comboDevices->CurrentIndex() == 0)
    {
        UpdateInputDevices();
        return;
    }

    IParamPackageList * devices = m_emulatedController->GetMappedDevicesPtr();
    UpdateInputDevices();

    if (devices->GetCount() == 0)
    {
        return;
    }

    if (devices->GetCount() > 2) 
    {
        m_comboDevices->SelectItem(0);
        return;
    }

    const IParamPackage & firstDevice = devices->GetParamPackage(0);
    const std::string first_engine = firstDevice.GetString("engine", "");
    const std::string first_guid = firstDevice.GetString("guid", "");
    const int32_t first_port = firstDevice.GetInt("port", 0);
    const int32_t first_pad = firstDevice.GetInt("pad", 0);

    if (devices->GetCount() == 1) 
    {
        uint32_t device_index = 0;
        const uint32_t device_count = m_inputDeviceList.GetCount();

        for (uint32_t i = 0; i < device_count; ++i) 
        {
            const IParamPackage & param = m_inputDeviceList.GetParamPackage(i);
            if (param.GetString("engine", "") == first_engine &&
                param.GetString("guid", "") == first_guid &&
                param.GetInt("port", 0) == first_port &&
                param.GetInt("pad", 0) == first_pad)
            {
                device_index = i;
                break;
            }
        }

        m_comboDevices->SelectItem(device_index);
        return;
    }

    const IParamPackage & secondDevice = devices->GetParamPackage(1);
    const std::string second_engine = secondDevice.GetString("engine", "");
    const std::string second_guid = secondDevice.GetString("guid", "");
    const int32_t second_port = secondDevice.GetInt("port", 0);

    const bool is_keyboard_mouse = (first_engine == "keyboard" || first_engine == "mouse") && (second_engine == "keyboard" || second_engine == "mouse");

    if (is_keyboard_mouse) 
    {
        m_comboDevices->SelectItem(2);
        return;
    }

    const bool is_engine_equal = first_engine == second_engine;
    const bool is_port_equal = first_port == second_port;

    if (is_engine_equal && is_port_equal)
    {
        uint32_t device_index = 0;
        for (uint32_t i = 0, n = m_inputDeviceList.GetCount(); i < n; i++)
        {
            IParamPackage & device = m_inputDeviceList.GetParamPackage(i);
            const bool is_guid_valid = (strcmp(device.GetString("guid", ""), first_guid.c_str()) == 0 && strcmp(device.GetString("guid2", ""), second_guid.c_str()) == 0) || (strcmp(device.GetString("guid", ""), second_guid.c_str()) == 0 && strcmp(device.GetString("guid2", ""), first_guid.c_str()) == 0);
            if (strcmp(device.GetString("engine", ""), first_engine.c_str()) == 0 && is_guid_valid && device.GetInt("port", 0) == first_port)
            {
                device_index = i;
                break;
            }
        }
        m_comboDevices->SelectItem(device_index);
    }
    else
    {
        m_comboDevices->SelectItem(0);
    }
}

void InputConfigPlayer::UpdateInputDevices(void)
{
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_page.GetElementByID("comboDevices"), IID_ICOMBOBOX);
    if (!interfacePtr)
    {
        return;
    }
    std::shared_ptr<IComboBox> comboBox = std::static_pointer_cast<IComboBox>(interfacePtr);
    comboBox->ClearContents();
    for (uint32_t i = 0, n = m_inputDeviceList.GetCount(); i < n; i++)
    {
        IParamPackage & device = m_inputDeviceList.GetParamPackage(i);
        const char * display = device.GetString("display", "Unknown");
        comboBox->AddItem(display, display);
    }
}

void InputConfigPlayer::UpdateUI()
{
    for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
    {
        ParamPackage param(m_emulatedController->GetButtonParamPtr(i));
        if (m_buttonMap[i].IsValid())
        {
            m_buttonMap[i].SetText(ButtonToText(param).c_str());
        }
    }
    const ParamPackage ZL_param(m_emulatedController->GetButtonParamPtr((uint32_t)NativeButtonValues::ZL));
    if (ZL_param->Has("threshold")) {
        const int button_threshold = static_cast<int>(ZL_param->GetFloat("threshold", 0.5f) * 100.0f);
        SciterElement sliderZLThreshold = m_page.GetElementByID("sliderZLThreshold");
        if (sliderZLThreshold.IsValid())
        {
            sliderZLThreshold.SetValue(button_threshold);
        }
    }

    const ParamPackage ZR_param(m_emulatedController->GetButtonParamPtr((uint32_t)NativeButtonValues::ZR));
    if (ZR_param->Has("threshold")) 
    {
        const int button_threshold = static_cast<int>(ZR_param.GetFloat("threshold", 0.5f) * 100.0f);
        SciterElement sliderZRThreshold = m_page.GetElementByID("sliderZRThreshold");
        if (sliderZRThreshold.IsValid())
        {
            sliderZRThreshold.SetValue(button_threshold);
        }
    }

    for (int i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
    {
        if (m_motionMap[i].IsValid())
        {
            ParamPackage param(m_emulatedController->GetMotionParamPtr(i));
            m_motionMap[i].SetText(ButtonToText(param).c_str());
        }
    }

    for (int analog_id = 0; analog_id < (size_t)NativeAnalogValues::NumAnalogs; ++analog_id)
    {
        const ParamPackage param(m_emulatedController->GetStickParamPtr(analog_id));
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id)
        {
            SciterElement & analog_button = m_analogMapButtons[analog_id][sub_button_id];
            if (!analog_button.IsValid()) 
            {
                continue;
            }
            analog_button.SetText(AnalogToText(param, analog_sub_buttons[sub_button_id]).c_str());
        }

        if (m_analogMapModifierButton[analog_id].IsValid())
        {
            m_analogMapModifierButton[analog_id].SetText(ButtonToText(ParamPackage(param->GetString("modifier", ""))).c_str());
        }

        const SciterElement & deadzone_label = m_analogMapDeadzoneLabel[analog_id];
        const SciterElement & deadzone_slider = m_analogMapDeadzoneSlider[analog_id];
        const SciterElement & modifier_groupbox = m_analogMapModifierGroupbox[analog_id];
        const SciterElement & modifier_label = m_analogMapModifierLabel[analog_id];
        const SciterElement & modifier_slider = m_analogMapModifierSlider[analog_id];
        const SciterElement & range_groupbox = m_analogMapRangeGroupbox[analog_id];
        const SciterElement & range_spinbox = m_analogMapRangeSpinbox[analog_id];

        const bool is_controller = m_operatingSystem.IsController(param);
        if (deadzone_label.IsValid())
        {
            deadzone_label.SetStyleAttribute("display", is_controller ? "" : "none");
        }
        if (deadzone_slider.IsValid())
        {
            deadzone_slider.SetStyleAttribute("display", is_controller ? "" : "none");
        }
        if (modifier_groupbox.IsValid())
        {
            modifier_groupbox.SetStyleAttribute("display", !is_controller ? "" : "none");
        }
        if (modifier_label.IsValid())
        {
            modifier_label.SetStyleAttribute("display", !is_controller ? "" : "none");
        }
        if (modifier_slider.IsValid())
        {
            modifier_slider.SetStyleAttribute("display", !is_controller ? "" : "none");
        }
        if (range_groupbox.IsValid())
        {
            range_groupbox.SetStyleAttribute("display", is_controller ? "" : "none");
        }

        int slider_value;
        if (is_controller) 
        {
            slider_value = (int)(param.GetFloat("deadzone", 0.15f) * 100);
            if (deadzone_label.IsValid())
            {
                deadzone_label.SetText(stdstr_f("Deadzone: %d%%", slider_value).c_str());
            }
            if (deadzone_slider.IsValid())
            {
                deadzone_slider.SetValue(slider_value);
            }
            if (range_spinbox.IsValid())
            {
                range_spinbox.SetValue((int)(param.GetFloat("range", 0.95f) * 100));
            }
        }
        else
        {
            slider_value = (int)(param.GetFloat("modifier_scale", 0.5f) * 100);
            if (modifier_label.IsValid())
            {
                modifier_label.SetText(stdstr_f("Modifier Range: %d%%", slider_value).c_str());
            }
            if (modifier_slider.IsValid())
            {
                modifier_slider.SetValue(slider_value);
            }
        }
    }
}

std::string InputConfigPlayer::ButtonToText(const IParamPackage& param)
{
    if (!param.Has("engine")) 
    {
        return "[not set]";
    }

    const std::string toggle = param.GetBool("toggle", false) ? "~" : "";
    const std::string inverted = param.GetBool("inverted", false) ? "!" : "";
    const std::string invert = std::string(param.GetString("invert", "+")) == "-" ? "-" : "";
    const std::string turbo = param.GetBool("turbo", false) ? "$" : "";

    const ButtonNames common_button_name = m_operatingSystem.GetButtonName(param);

    // Retrieve the names from Qt
    if (std::string(param.GetString("engine", "")) == "keyboard") 
    {
        const std::string button_str = GetKeyName(param.GetInt("code", 0));
        return stdstr_f("%s%s%s%s", turbo.c_str(), toggle.c_str(), inverted.c_str(), button_str.c_str());
    }

    if (common_button_name == ButtonNames::Invalid) 
    {
        return "[invalid]";
    }

    if (common_button_name == ButtonNames::Engine) 
    {
        return param.GetString("engine", "");
    }

    if (common_button_name == ButtonNames::Value) 
    {
        if (param.Has("hat")) 
        {
            return stdstr_f("%s%s%sHat %s", turbo.c_str(), toggle.c_str(), inverted.c_str(), param.GetString("direction", ""));
        }
        if (param.Has("axis")) 
        {
            return stdstr_f("%s%s%sAxis %s", toggle.c_str(), inverted.c_str(), invert.c_str(), param.GetString("axis", ""));
        }
        if (param.Has("axis_x") && param.Has("axis_y") && param.Has("axis_z")) 
        {
            return stdstr_f("%s%sAxis %s,%s,%s", toggle.c_str(), inverted.c_str(), param.GetString("axis_x", ""), param.GetString("axis_y", ""), param.GetString("axis_z", ""));
        }
        if (param.Has("motion")) 
        {
            return stdstr_f("%s%sMotion %s", toggle.c_str(), inverted.c_str(), param.GetString("motion", ""));
        }
        if (param.Has("button")) 
        {
            return stdstr_f("%s%s%sButton %s", turbo.c_str(), toggle.c_str(), inverted.c_str(), param.GetString("button", ""));
        }
    }

    std::string button_name = GetButtonName(common_button_name);
    if (param.Has("hat"))
    {
        return stdstr_f("%s%s%sHat %s", turbo.c_str(), toggle.c_str(), inverted.c_str(), button_name.c_str());
    }
    if (param.Has("axis"))
    {
        return stdstr_f("%s%s%sAxis %s", turbo.c_str(), toggle.c_str(), inverted.c_str(), button_name.c_str());
    }
    if (param.Has("motion"))
    {
        return stdstr_f("%s%sAxis %s", toggle.c_str(), inverted.c_str(), button_name.c_str());
    }
    if (param.Has("button")) 
    {
        return stdstr_f("%s%s3Button %s", turbo.c_str(), toggle.c_str(), inverted.c_str(), button_name.c_str());
    }
    return "[unknown]";
}

std::string InputConfigPlayer::AnalogToText(const IParamPackage& param, const std::string& dir)
{
    if (!param.Has("engine"))
    {
        return "[not set]";
    }

    if (std::string(param.GetString("engine", "")) == "analog_from_button") 
    {
        return ButtonToText(ParamPackage({ param.GetString(dir.c_str(), "") }));
    }

    if (!param.Has("axis_x") || !param.Has("axis_y"))
    {
        return "[unknown]"  ;
    }

    if (dir == "modifier") 
    {
        return "[unused]";
    }

    const bool invert_x = std::string(param.GetString("invert_x", "+")) == "-";
    const bool invert_y = std::string(param.GetString("invert_y", "+")) == "-";
    if (dir == "left")
    {
        return stdstr_f("Axis %s%s", param.GetString("axis_x", ""), invert_x ? "+" : "-");
    }
    if (dir == "right") 
    {
        return stdstr_f("Axis %s%s", param.GetString("axis_x", ""), invert_x ? "-" : "+");
    }
    if (dir == "up") 
    {
        return stdstr_f("Axis %s%s", param.GetString("axis_y", ""), invert_y ? "-" : "+");
    }
    if (dir == "down") 
    {
        return stdstr_f("Axis %s%s", param.GetString("axis_y", ""), invert_y ? "+" : "-");
    }
    return "[unknown]";
}

int InputConfigPlayer::GetIndexFromControllerType(NpadStyleIndex type) const 
{
    const std::vector<std::pair<int, NpadStyleIndex>>::const_iterator it = std::find_if(index_controller_type_pairs.begin(), index_controller_type_pairs.end(), [type](const std::pair<int, NpadStyleIndex>& pair) { return pair.second == type; });
    if (it == index_controller_type_pairs.end())
    {
        return -1;
    }

    return it->first;
}

NpadStyleIndex InputConfigPlayer::GetControllerTypeFromIndex(int index) const
{
    const std::vector<std::pair<int, NpadStyleIndex>>::const_iterator it = std::find_if(index_controller_type_pairs.begin(), index_controller_type_pairs.end(), [index](const std::pair<int, NpadStyleIndex>& pair) { return pair.first == index; });
    if (it == index_controller_type_pairs.end())
    {
        return NpadStyleIndex::Fullkey;
    }
    return it->second;
}

void InputConfigPlayer::SetConnectableControllers() 
{
    if (m_comboControllerType == nullptr)
    {
        return;
    }
    const NpadStyleSet npad_style_set = m_operatingSystem.GetSupportedStyleTag();
    index_controller_type_pairs.clear();
    m_comboControllerType->ClearContents();

    typedef std::vector<std::pair<NpadStyleIndex, std::string>> Controllers;
    Controllers availableControllers;

    if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Fullkey) != 0)
    {
        availableControllers.emplace_back(NpadStyleIndex::Fullkey, "Pro Controller");
    }

    if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::JoyDual) != 0)
    {
        availableControllers.emplace_back(NpadStyleIndex::JoyconDual, "Dual Joycons");
    }

    if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::JoyLeft) != 0)
    {
        availableControllers.emplace_back(NpadStyleIndex::JoyconLeft, "Left Joycon");
    }

    if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::JoyRight) != 0)
    {
        availableControllers.emplace_back(NpadStyleIndex::JoyconRight, "Right Joycon");
    }

    if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Gc) != 0)
    {
        availableControllers.emplace_back(NpadStyleIndex::GameCube, "GameCube Controller");
    }

    if (m_controllerIndex == NpadIdType::Player1 && (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Handheld) != 0))
    {
        availableControllers.emplace_back(NpadStyleIndex::Handheld, "Handheld");
    }

    // Disable all unsupported controllers
    if (uiSettings.enableAllControllers) 
    {
        if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Palma) != 0)
        {
            availableControllers.emplace_back(NpadStyleIndex::Pokeball, "Poke Ball Plus");
        }

        if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Palma) != 0)
        {
            availableControllers.emplace_back(NpadStyleIndex::NES, "NES Controller");
        }

        if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Lucia) != 0)
        {
            availableControllers.emplace_back(NpadStyleIndex::SNES, "SNES Controller");
        }

        if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Lagoon) != 0)
        {
            availableControllers.emplace_back(NpadStyleIndex::N64, "N64 Controller");
        }

        if (((uint32_t)npad_style_set & (uint32_t)NpadStyleSet::Lager) != 0)
        {
            availableControllers.emplace_back(NpadStyleIndex::SegaGenesis, "Sega Genesis");
        }
    }

    for (Controllers::const_iterator itr = availableControllers.begin(); itr != availableControllers.end(); itr ++)
    {
        index_controller_type_pairs.emplace_back(m_comboControllerType->GetCount(), itr->first);
        m_comboControllerType->AddItem(itr->second.c_str(), itr->second.c_str());
    }
}

void InputConfigPlayer::ControllerTypeChanged()
{
    UpdateControllerAvailableButtons();
    UpdateControllerEnabledButtons();
    UpdateControllerButtonNames();
    UpdateMotionButtons();
    const NpadStyleIndex type = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());
    if (m_controllerIndex == NpadIdType::Player1) 
    {
        bool is_connected = m_emulatedController->IsConnected(true);

        m_emulatedControllerPlayer.SetNpadStyleIndex(type);
        m_emulatedControllerHandheld.SetNpadStyleIndex(type);
        if (is_connected)
        {
            if (type == NpadStyleIndex::Handheld) 
            {
                m_emulatedControllerPlayer.Disconnect();
                m_emulatedControllerHandheld.Connect(true);
                m_emulatedController = &m_emulatedControllerHandheld;
            }
            else 
            {
                m_emulatedControllerHandheld.Disconnect();
                m_emulatedControllerPlayer.Connect(true);
                m_emulatedController = &m_emulatedControllerPlayer;
            }
        }
    }
    m_emulatedController->SetNpadStyleIndex(type);
}

void InputConfigPlayer::UpdateMappingWithDefaults()
{
    if (m_comboDevices->CurrentIndex() == 0)
    {
        return;
    }

    for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
    {
        if (!m_buttonMap[i].IsValid()) 
        {
            continue;
        }
        m_emulatedController->SetButtonParam(i, ParamPackage());
    }

    for (int analog_id = 0; analog_id < (size_t)NativeAnalogValues::NumAnalogs; analog_id++)
    {
        bool found = false;
        for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; sub_button_id++)
        {
            SciterElement & analog_button = m_analogMapButtons[analog_id][sub_button_id];
            if (!analog_button.IsValid()) 
            {
                continue;
            }
            found = true;
            break;
        }
        if (found)
        {
            m_emulatedController->SetStickParam(analog_id, ParamPackage());
        }
    }
    
    for (int i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
    {
        if (m_motionMap[i].IsValid())
        {
            continue;
        }
        m_emulatedController->SetMotionParam(i, ParamPackage());
    }

    // Reset keyboard or mouse bindings
    if (m_comboDevices->CurrentIndex() == 1 || m_comboDevices->CurrentIndex() == 2) 
    {
        for (uint32_t i = 0, n = (uint32_t)NativeButtonValues::NumButtons; i < n; i++)
        {
            m_emulatedController->SetButtonParam(i, ParamPackage(GenerateKeyboardParam(default_buttons[i]) ));
        }
        for (int analog_id = 0; analog_id < (size_t)NativeAnalogValues::NumAnalogs; ++analog_id)
        {
            ParamPackage analog_param;
            for (int sub_button_id = 0; sub_button_id < ANALOG_SUB_BUTTONS_NUM; ++sub_button_id)
            {
                ParamPackage params{ GenerateKeyboardParam( default_analogs[analog_id][sub_button_id]) };
                SetAnalogParam(params, analog_param, analog_sub_buttons[sub_button_id]);
            }

            analog_param.Set("modifier", GenerateKeyboardParam(default_stick_mod[analog_id]));
            m_emulatedController->SetStickParam(analog_id, analog_param);
        }

        for (int i = 0, n = (uint32_t)NativeMotionValues::NumMotions; i < n; i++)
        {
            m_emulatedController->SetMotionParam(i, ParamPackage( GenerateKeyboardParam(default_motions[i]) ));
        }

        // If mouse is selected we want to override with mappings from the driver
        if (m_comboDevices->CurrentIndex() == 1) 
        {
            UpdateUI();
            return;
        }
    }

    // Reset controller bindings
    const IParamPackage & device = m_inputDeviceList.GetParamPackage(m_comboDevices->CurrentIndex());
    ButtonMappingListPtr button_mappings(m_operatingSystem.GetButtonMappingForDevice(device));
    ButtonMappingListPtr analog_mappings(m_operatingSystem.GetAnalogMappingForDevice(device));
    ButtonMappingListPtr motion_mappings(m_operatingSystem.GetMotionMappingForDevice(device));
    
    for (uint32_t i = 0; i < button_mappings->GetCount(); ++i)
    {
        const uint32_t index = button_mappings->GetIndex(i);
        m_emulatedController->SetButtonParam(index, button_mappings->GetParamPackage(i));
    }

    for (uint32_t i = 0; i < analog_mappings->GetCount(); ++i)
    {
        const uint32_t index = analog_mappings->GetIndex(i);
        m_emulatedController->SetStickParam(index, analog_mappings->GetParamPackage(i));
    }

    for (uint32_t i = 0; i < motion_mappings->GetCount(); ++i)
    {
        const uint32_t index = motion_mappings->GetIndex(i);
        m_emulatedController->SetMotionParam(index, motion_mappings->GetParamPackage(i));
    }
    UpdateUI();
}

void InputConfigPlayer::HandleClick(SciterElement & button, uint32_t buttonId, PollingInputType type)
{
    if (m_timeoutTimerActive) 
    {
        return;
    }
    if (button == m_motionMap[0] || button == m_motionMap[1])
    {
        button.SetText("Shake!");
    }
    else
    {
        button.SetText("[waiting]");
    }
    button.SetState(SciterElement::STATE_FOCUS, 0, true);

    m_pollingButtonId = buttonId;
    m_pollingType = type;
    m_operatingSystem.BeginMapping(type);
    if (m_pollingType == PollingInputType::Button)
    {
        UpdateButtonState();
    }

    m_timeoutTimerActive = true;
    m_page.SetTimer(4000, (uint32_t*)TIMER_TIMEOUT);
    m_page.SetTimer(25, (uint32_t*)TIMER_POLL);
}

void InputConfigPlayer::RefreshStickUi()
{
    if (!m_callbackEnabled || !m_page.IsValid())
    {
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (m_stickUiThrottleLast != std::chrono::steady_clock::time_point{} &&
        (now - m_stickUiThrottleLast) < std::chrono::milliseconds(50))
    {
        return;
    }
    m_stickUiThrottleLast = now;
    m_stickValues = m_emulatedController->GetSticksValues();

    // Y axis is inverted
    m_stickValues.status[(size_t)NativeAnalogValues::LStick].y.value = -m_stickValues.status[(size_t)NativeAnalogValues::LStick].y.value;
    m_stickValues.status[(size_t)NativeAnalogValues::LStick].y.raw_value = -m_stickValues.status[(size_t)NativeAnalogValues::LStick].y.raw_value;
    m_stickValues.status[(size_t)NativeAnalogValues::RStick].y.value = -m_stickValues.status[(size_t)NativeAnalogValues::RStick].y.value;
    m_stickValues.status[(size_t)NativeAnalogValues::RStick].y.raw_value = -m_stickValues.status[(size_t)NativeAnalogValues::RStick].y.raw_value;
    SciterElement controllerSvg = GetControllerSvg();
    if (!controllerSvg.IsValid())
    {
        return;
    }
    UpdateStickDisplay(controllerSvg, NativeAnalogValues::LStick);
    UpdateStickDisplay(controllerSvg, NativeAnalogValues::RStick);

    controllerSvg.Update(true);
    m_sciterUI.UpdateWindow(m_page.GetElementHwnd(true));
}

void InputConfigPlayer::ControllerEventCallback(ControllerTriggerType type)
{
    if (!m_callbackEnabled)
    {
        return;
    }
    switch (type) {
    case ControllerTriggerType::Button:
    case ControllerTriggerType::Trigger:
        m_emulatedController->GetButtonsStatus(m_buttonValues, sizeof(m_buttonValues)/sizeof(m_buttonValues[0]));
        UpdateButtonState();
        break;
    case ControllerTriggerType::Stick:
        RefreshStickUi();
        break;
    case ControllerTriggerType::Motion:
        UpdateMotionCube();
        break;
    }
}

void InputConfigPlayer::UpdateControllerGraphic(NpadStyleIndex layout)
{
    if (layout == m_displayedControllerLayout)
    {
        return;
    }

    SciterElement host = m_page.GetElementByID("controllerGraphics");
    if (!host.IsValid())
    {
        return;
    }

    const char * resource = GetControllerSvgResource(layout);
    if (resource == nullptr)
    {
        host.SetHTML((const uint8_t *)"", 0, SciterElement::SIH_REPLACE_CONTENT);
        m_displayedControllerLayout = layout;
        return;
    }

    const std::string & svg = LoadCachedControllerSvg(m_sciterUI, resource);
    if (svg.empty())
    {
        return;
    }
    host.SetHTML(reinterpret_cast<const uint8_t *>(svg.data()), svg.size(), SciterElement::SIH_REPLACE_CONTENT);
    m_displayedControllerLayout = layout;
}

void InputConfigPlayer::UpdateControllerAvailableButtons()
{
    if (m_comboControllerType == nullptr)
    {
        return;
    }
    NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());
    UpdateControllerGraphic(layout);

    const std::array<SciterElement, 16> layout_show = {
        m_page.GetElementByID("buttonShoulderButtonsSLSRLeft"),
        m_page.GetElementByID("buttonShoulderButtonsSLSRRight"),
        m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget"),
        m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget2"),
        m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget3"),
        m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget4"),
        m_page.GetElementByID("buttonShoulderButtonsLeft"),
        m_page.GetElementByID("buttonMiscButtonsMinusScreenshot"),
        m_page.GetElementByID("bottomLeft"),
        m_page.GetElementByID("bottomLeftSpacer"),
        m_page.GetElementByID("buttonShoulderButtonsRight"),
        m_page.GetElementByID("buttonMiscButtonsPlusHome"),
        m_page.GetElementByID("bottomRight"),
        m_page.GetElementByID("bottomRightSpacer"),
        m_page.GetElementByID("buttonMiscButtonsMinusGroup"),
        m_page.GetElementByID("buttonMiscButtonsScreenshotGroup"),
    };

    for (const SciterElement & el : layout_show) 
    {
        if (!el.IsValid())
        {
            continue;
        }
        el.SetStyleAttribute("display", "");
    }

    std::vector<SciterElement> layout_hidden;
    switch (layout) {
    case NpadStyleIndex::Fullkey:
        layout_hidden = {
            m_page.GetElementByID("bottomLeftSpacer"),
            m_page.GetElementByID("bottomRightSpacer"),
            m_page.GetElementByID("buttonShoulderButtonsSLSRLeft"),
            m_page.GetElementByID("buttonShoulderButtonsSLSRRight"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget2"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget4"),
        };
        break;
    case NpadStyleIndex::Handheld:
        layout_hidden = {
            m_page.GetElementByID("bottomLeftSpacer"),
            m_page.GetElementByID("bottomRightSpacer"),
            m_page.GetElementByID("buttonShoulderButtonsSLSRLeft"),
            m_page.GetElementByID("buttonShoulderButtonsSLSRRight"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget2"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget4"),
        };
        break;
    case NpadStyleIndex::JoyconDual:
        layout_hidden = {
            m_page.GetElementByID("bottomLeftSpacer"),
            m_page.GetElementByID("bottomRightSpacer"),
        };
        break;
    case NpadStyleIndex::JoyconLeft:
        layout_hidden = {
            m_page.GetElementByID("buttonShoulderButtonsSLSRRight"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget2"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget3"),
            m_page.GetElementByID("buttonShoulderButtonsRight"),
            m_page.GetElementByID("buttonMiscButtonsPlusHome"),
            m_page.GetElementByID("bottomLeftSpacer"),
            m_page.GetElementByID("bottomRight"),
        };
        break;
    case NpadStyleIndex::JoyconRight:
        layout_hidden = {
            m_page.GetElementByID("buttonShoulderButtonsSLSRLeft"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget4"),
            m_page.GetElementByID("buttonShoulderButtonsLeft"),
            m_page.GetElementByID("buttonMiscButtonsMinusScreenshot"),
            m_page.GetElementByID("bottomLeft"),
            m_page.GetElementByID("bottomRightSpacer"),
        };
        break;
    case NpadStyleIndex::GameCube:
        layout_hidden = {
            m_page.GetElementByID("buttonShoulderButtonsSLSRLeft"),
            m_page.GetElementByID("buttonShoulderButtonsSLSRRight"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget2"),
            m_page.GetElementByID("horizontalSpacerShoulderButtonsWidget4"),
            m_page.GetElementByID("buttonMiscButtonsMinusGroup"),
            m_page.GetElementByID("buttonMiscButtonsScreenshotGroup"),
            m_page.GetElementByID("bottomLeftSpacer"),
            m_page.GetElementByID("bottomRightSpacer"),
        };
        break;
    default:
        break;
    }

    for (const SciterElement & el : layout_hidden)
    {
        if (!el.IsValid())
        {
            continue;
        }
        el.SetStyleAttribute("display", "none");
    }
}

void InputConfigPlayer::UpdateControllerEnabledButtons() 
{
    if (m_comboControllerType == nullptr)
    {
        return;
    }
    NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());

    // List of all the widgets that will be disabled by any of the following layouts that need
    // "enabled" after the controller type changes
    const std::array<SciterElement, 3> layout_enable = {
        m_page.GetElementByID("buttonLStickPressedGroup"),
        m_page.GetElementByID("groupRStickPressed"),
        m_page.GetElementByID("buttonShoulderButtonsButtonLGroup"),
    };

    for (const SciterElement & el : layout_enable)
    {
        if (!el.IsValid())
        {
            continue;
        }
        el.SetState(0, SciterElement::STATE_DISABLED, true);
    }

    std::vector<SciterElement> layout_disable;
    switch (layout) {
    case NpadStyleIndex::Fullkey:
    case NpadStyleIndex::JoyconDual:
    case NpadStyleIndex::Handheld:
    case NpadStyleIndex::JoyconLeft:
    case NpadStyleIndex::JoyconRight:
        break;
    case NpadStyleIndex::GameCube:
        layout_disable = {
            m_page.GetElementByID("buttonHome"),
            m_page.GetElementByID("buttonLStickPressedGroup"),
            m_page.GetElementByID("groupRStickPressed"),
            m_page.GetElementByID("buttonShoulderButtonsButtonLGroup"),
        };
        break;
    default:
        break;
    }

    for (const SciterElement & el : layout_disable)
    {
        if (!el.IsValid())
        {
            continue;
        }
        el.SetState(SciterElement::STATE_DISABLED, 0, true);
    }
}

void InputConfigPlayer::UpdateControllerButtonNames() 
{
    if (m_comboControllerType == nullptr)
    {
        return;
    }
    NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());
    SciterElement el;

    switch (layout) {
    case NpadStyleIndex::Fullkey:
    case NpadStyleIndex::JoyconDual:
    case NpadStyleIndex::Handheld:
    case NpadStyleIndex::JoyconLeft:
    case NpadStyleIndex::JoyconRight:
        el = m_page.GetElementByID("buttonMiscButtonsPlusGroup");
        if (el.IsValid())
        {
            el.SetText("Plus");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsButtonZLGroup");
        if (el.IsValid())
        {
            el.SetText("ZL");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsZRGroup");
        if (el.IsValid())
        {
            el.SetText("ZR");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsRGroup");
        if (el.IsValid())
        {
            el.SetText("R");
        }
        el = m_page.GetElementByID("LStick");
        if (el.IsValid())
        {
            el.SetText("Left Stick");
        }
        el = m_page.GetElementByID("RStick");
        if (el.IsValid())
        {
            el.SetText("Right Stick");
        }
        break;
    case NpadStyleIndex::GameCube:
        el = m_page.GetElementByID("buttonMiscButtonsPlusGroup");
        if (el.IsValid())
        {
            el.SetText("Start / Pause");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsButtonZLGroup");
        if (el.IsValid())
        {
            el.SetText("L");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsZRGroup");
        if (el.IsValid())
        {
            el.SetText("R");
        }
        el = m_page.GetElementByID("buttonShoulderButtonsRGroup");
        if (el.IsValid())
        {
            el.SetText("Z");
        }
        el = m_page.GetElementByID("LStick");
        if (el.IsValid())
        {
            el.SetText("Control Stick");
        }
        el = m_page.GetElementByID("RStick->setTitle");
        if (el.IsValid())
        {
            el.SetText("C-Stick");
        }
        break;
    }
}

void InputConfigPlayer::UpdateButtonState()
{
    if (!m_callbackEnabled || !m_page.IsValid())
    {
        return;
    }
    struct BtnMap 
    {
        uint32_t idx; 
        const char* sel; 
    };

    static const BtnMap kBtnMap[] = {
        {(uint32_t)NativeButtonValues::A,          ".buttons-face .button.A" },
        {(uint32_t)NativeButtonValues::B,          ".buttons-face .button.B" },
        {(uint32_t)NativeButtonValues::X,          ".buttons-face .button.X" },
        {(uint32_t)NativeButtonValues::Y,          ".buttons-face .button.Y" },
        {(uint32_t)NativeButtonValues::DUp,        ".dpad .up"    },
        {(uint32_t)NativeButtonValues::DDown,      ".dpad .down"  },
        {(uint32_t)NativeButtonValues::DLeft,      ".dpad .left"  },
        {(uint32_t)NativeButtonValues::DRight,     ".dpad .right" },
        {(uint32_t)NativeButtonValues::ZL,         ".trigger.ZL"  },
        {(uint32_t)NativeButtonValues::ZR,         ".trigger.ZR"  },
        {(uint32_t)NativeButtonValues::L,          ".trigger.L"  },
        {(uint32_t)NativeButtonValues::R,          ".trigger.R"  },
        {(uint32_t)NativeButtonValues::Minus,      ".button.minus" },
        {(uint32_t)NativeButtonValues::Plus,       ".button.plus" },
        {(uint32_t)NativeButtonValues::Screenshot, ".button.screenshot" },
        {(uint32_t)NativeButtonValues::Home,       ".button.home" },
        {(uint32_t)NativeButtonValues::SLLeft,     ".button.SL-left"},
        {(uint32_t)NativeButtonValues::SRLeft,     ".button.SR-left"},
        {(uint32_t)NativeButtonValues::SLRight,    ".button.SL-right"},
        {(uint32_t)NativeButtonValues::SRRight,    ".button.SR-right"},
    };

    SciterElement controllerSvg = GetControllerSvg();
    if (!controllerSvg.IsValid())
    {
        return;
    }

    for (const BtnMap & m : kBtnMap)
    {
        const button_status_t & bs = m_buttonValues[m.idx];
        SciterElements els = controllerSvg.FindAll(m.sel);
        if (els.empty())
        {
            continue;
        }

        for (SciterElement & el : els)
        {
            if (m_pollingType == PollingInputType::Button && m.idx == m_pollingButtonId)
            {
                el.SetAttribute("data-setting", "1");
            }
            else
            {
                el.RemoveAttribute("data-setting");
            }

            if (bs.value)
            {
                el.SetAttribute("data-pressed", "1");
            }
            else
            {
                el.RemoveAttribute("data-pressed");
            }
        }
    }

    controllerSvg.Update(true);
    m_sciterUI.UpdateWindow(m_page.GetElementHwnd(true));
}

void InputConfigPlayer::UpdateMotionButtons()
{
    if (m_comboControllerType == nullptr)
    {
        return;
    }

    const NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());

    bool show_left = false;
    bool show_right = false;
    bool show_bar = false;

    switch (layout)
    {
    case NpadStyleIndex::Fullkey:
    case NpadStyleIndex::JoyconLeft:
    case NpadStyleIndex::Handheld:
        show_left = true;
        show_bar = true;
        break;
    case NpadStyleIndex::JoyconRight:
        show_right = true;
        show_bar = true;
        break;
    case NpadStyleIndex::GameCube:
        break;
    case NpadStyleIndex::JoyconDual:
    default:
        show_left = true;
        show_right = true;
        show_bar = true;
        break;
    }

    if (m_motionGroup[0].IsValid())
    {
        m_motionGroup[0].SetStyleAttribute("display", show_left ? "" : "none");
    }
    if (m_motionGroup[1].IsValid())
    {
        m_motionGroup[1].SetStyleAttribute("display", show_right ? "" : "none");
    }
    if (m_centerButtonsMotion.IsValid())
    {
        m_centerButtonsMotion.SetStyleAttribute("display", show_bar ? "" : "none");
        const char* motion_class = show_bar && show_left != show_right ? "center-buttons-motion single-motion" : "center-buttons-motion";
        m_centerButtonsMotion.SetAttribute("class", motion_class);
    }
}

void InputConfigPlayer::UpdateMotionCube()
{
    if (!m_callbackEnabled || !m_page.IsValid())
    {
        return;
    }
    if (m_comboControllerType == nullptr)
    {
        return;
    }
    const NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());
    if (layout == NpadStyleIndex::GameCube)
    {
        return;
    }

    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (m_motionUiThrottleLast != std::chrono::steady_clock::time_point{} && (now - m_motionUiThrottleLast) < std::chrono::milliseconds(50))
    {
        return;
    }
    m_motionUiThrottleLast = now;

    m_motionValues = m_emulatedController->GetMotions();

    SciterElement svg = GetControllerSvg();
    if (!svg.IsValid())
    {
        return;
    }

    switch (layout)
    {
    case NpadStyleIndex::JoyconDual:
        PaintMotionCubeGroup(svg.GetElementByID("motion-left"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionLeft].euler, 20.0f);
        PaintMotionCubeGroup(svg.GetElementByID("motion-right"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionRight].euler, 20.0f);
        break;
    case NpadStyleIndex::JoyconRight:
        PaintMotionCubeGroup(svg.FindFirst(".motion"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionRight].euler, 20.0f);
        break;
    case NpadStyleIndex::JoyconLeft:
        PaintMotionCubeGroup(svg.FindFirst(".motion"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionLeft].euler, 20.0f);
        break;
    case NpadStyleIndex::Fullkey:
        PaintMotionCubeGroup(svg.FindFirst(".motion"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionLeft].euler, 15.0f);
        break;
    case NpadStyleIndex::Handheld:
        PaintMotionCubeGroup(svg.FindFirst(".motion"), m_motionValues.motion[(uint32_t)NativeMotionValues::MotionLeft].euler, 15.0f);
        break;
    default:
        return;
    }

    svg.Update(true);
    m_sciterUI.UpdateWindow(m_page.GetElementHwnd(true));
}

void InputConfigPlayer::UpdateStickDisplay(const SciterElement& svg, NativeAnalogValues analog)
{
    if (analog != NativeAnalogValues::LStick && analog != NativeAnalogValues::RStick)
    {
        return;
    }
    if (!svg.IsValid())
    {
        return;
    }
    StickStatus & status = m_stickValues.status[(size_t)analog];

    SciterElement el = svg.FindFirst(analog == NativeAnalogValues::LStick ? ".left-stick-visual" : ".right-stick-visual");
    if (!el.IsValid())
    {
        return;
    }
    SciterElement range = el.FindFirst(".joystick-range");
    float radius = 45.0f;
    if (range.IsValid())
    {
        radius = strtof(range.GetAttribute("r").c_str(), nullptr);
    }
    SciterElement rawDot = el.FindFirst(".joystick-dot-raw");
    if (rawDot.IsValid())
    {
        rawDot.SetAttribute("cx", stdstr_f("%f", status.x.raw_value * radius).c_str());
        rawDot.SetAttribute("cy", stdstr_f("%f", status.y.raw_value * radius).c_str());
    }
    SciterElement rawCalibrated = el.FindFirst(".joystick-dot-calibrated");
    if (rawCalibrated.IsValid())
    {
        rawCalibrated.SetAttribute("cx", stdstr_f("%f", status.x.value * radius).c_str());
        rawCalibrated.SetAttribute("cy", stdstr_f("%f", status.y.value * radius).c_str());
    }
    SciterElement leftStick = analog == NativeAnalogValues::LStick ? svg.FindFirst(".left-stick-movable") : SciterElement();
    if (leftStick.IsValid())
    {
        leftStick.SetAttribute("transform", stdstr_f("translate(%f,%f)", status.x.value * 10.0f, status.y.value * 10.0f).c_str());
    }
    SciterElement rightStick = analog == NativeAnalogValues::RStick ? svg.FindFirst(".right-stick-movable") : SciterElement();
    if (rightStick.IsValid())
    {
        rightStick.SetAttribute("transform", stdstr_f("translate(%f,%f)", status.x.value * 9.5f, status.y.value * 9.5f).c_str());
    }

    SciterElement proStick;
    if (analog == NativeAnalogValues::LStick)
    {
        proStick = svg.FindFirst(".pro-left-stick");
    }
    else if (analog == NativeAnalogValues::RStick)
    {
        proStick = svg.FindFirst(".pro-right-stick");
    }

    if (proStick.IsValid())
    {
        SciterElement movable = proStick.FindFirst(".movable");
        SciterElement outerEll = proStick.FindFirst(".outer");
        SciterElement innerEll = proStick.FindFirst(".inner");
        SciterElement innerShift = proStick.FindFirst(".inner-shift");
        if (movable.IsValid() && outerEll.IsValid() && innerEll.IsValid() && innerShift.IsValid())
        {
            const float x = status.x.value;
            const float y = status.y.value;
            const float mag = std::min(1.0f, std::hypot(x, y));
            const float amp = std::max(0.0f, 1.0f - mag * 0.1f);
            const float angleDeg = std::atan2(y, x) * 180.0f / (float)(std::numbers::pi);
            const float scalar = 11.0f;
                
            movable.SetAttribute("transform", stdstr_f("translate(%f,%f) rotate(%f)", x * scalar, y * scalar, angleDeg).c_str());
            outerEll.SetAttribute("rx", stdstr_f("%f", 24.0f * amp).c_str());
            innerEll.SetAttribute("rx", stdstr_f("%f", 17.0f * amp).c_str());
            innerShift.SetAttribute("transform", stdstr_f("translate(%f,0)", ((24.0f - 17.0f) * 0.4f) * mag).c_str());
        }
    }
}

void InputConfigPlayer::DeadzoneSliderChanged(uint32_t analogId)
{
    ParamPackage param(m_emulatedController->GetStickParamPtr(analogId));
    const int slider_value = m_analogMapDeadzoneSlider[analogId].GetValue().GetValueInt();
    if (m_analogMapDeadzoneLabel[analogId].IsValid())
    {
        m_analogMapDeadzoneLabel[analogId].SetText(stdstr_f("Deadzone: %d%%",slider_value).c_str());
    }
    param.SetFloat("deadzone", slider_value / 100.0f);
    m_emulatedController->SetStickParam(analogId, param);

    SciterElement controllerSvg = GetControllerSvg();
    if (controllerSvg.IsValid())
    {
        SciterElement deadzoneEl = controllerSvg.FindFirst(analogId == 0 ? ".left-stick-visual .joystick-deadzone" : ".right-stick-visual .joystick-deadzone");
        if (deadzoneEl.IsValid())
        {
            deadzoneEl.SetAttribute("r", stdstr_f("%d", (int)((slider_value / 100.0f) * 45)).c_str());
        }
    }
}

const char * InputConfigPlayer::VibrationEnabledSettingId() const
{
    static const char * ids[] = {
        NXOsSetting::Player0VibrationEnabled, NXOsSetting::Player1VibrationEnabled,
        NXOsSetting::Player2VibrationEnabled, NXOsSetting::Player3VibrationEnabled,
        NXOsSetting::Player4VibrationEnabled, NXOsSetting::Player5VibrationEnabled,
        NXOsSetting::Player6VibrationEnabled, NXOsSetting::Player7VibrationEnabled,
    };
    const uint32_t index = static_cast<uint32_t>(m_controllerIndex);
    return index < 8 ? ids[index] : ids[0];
}

const char * InputConfigPlayer::VibrationStrengthSettingId() const
{
    static const char * ids[] = {
        NXOsSetting::Player0VibrationStrength, NXOsSetting::Player1VibrationStrength,
        NXOsSetting::Player2VibrationStrength, NXOsSetting::Player3VibrationStrength,
        NXOsSetting::Player4VibrationStrength, NXOsSetting::Player5VibrationStrength,
        NXOsSetting::Player6VibrationStrength, NXOsSetting::Player7VibrationStrength,
    };
    const uint32_t index = static_cast<uint32_t>(m_controllerIndex);
    return index < 8 ? ids[index] : ids[0];
}

void InputConfigPlayer::LoadVibrationControls()
{
    SettingsStore & settings = SettingsStore::GetInstance();
    if (m_vibrationEnabled.IsValid())
    {
        const bool enabled = settings.GetBool(VibrationEnabledSettingId());
        m_vibrationEnabled.SetState(enabled ? SciterElement::STATE_CHECKED : 0,
                                    enabled ? 0 : SciterElement::STATE_CHECKED, true);
    }
    if (m_vibrationStrength.IsValid())
    {
        m_vibrationStrength.SetValue(SciterValue(settings.GetInt(VibrationStrengthSettingId())));
    }
    UpdateVibrationStrengthDisplay();
    UpdateVibrationControlsEnabled();
}

void InputConfigPlayer::SaveVibrationControls()
{
    // Keep last-saved per-player values when globally disabled so they restore when re-enabled.
    if (!SettingsStore::GetInstance().GetBool(NXOsSetting::VibrationEnabled))
    {
        return;
    }
    SettingsStore & settings = SettingsStore::GetInstance();
    if (m_vibrationEnabled.IsValid())
    {
        settings.SetBool(VibrationEnabledSettingId(),
                         (m_vibrationEnabled.GetState() & SciterElement::STATE_CHECKED) != 0);
    }
    if (m_vibrationStrength.IsValid())
    {
        SciterValue value = m_vibrationStrength.GetValue();
        if (value.isInt())
        {
            settings.SetInt(VibrationStrengthSettingId(), value.GetValueInt());
        }
    }
}

void InputConfigPlayer::UpdateVibrationStrengthDisplay()
{
    if (!m_vibrationStrengthLabel.IsValid() || !m_vibrationStrength.IsValid())
    {
        return;
    }
    SciterValue value = m_vibrationStrength.GetValue();
    if (value.isInt())
    {
        m_vibrationStrengthLabel.SetText(stdstr_f("Strength: %d%%", value.GetValueInt()).c_str());
    }
}

void InputConfigPlayer::UpdateVibrationControlsEnabled()
{
    const bool globallyEnabled = SettingsStore::GetInstance().GetBool(NXOsSetting::VibrationEnabled);
    if (!globallyEnabled && m_vibrationStrengthPopup.IsValid())
    {
        m_sciterUI.PopupHide(m_vibrationStrengthPopup);
        if (m_vibrationStrengthPopupBtn.IsValid())
        {
            m_vibrationStrengthPopupBtn.RemoveClassName("open");
        }
    }

    auto setDisabled = [globallyEnabled](SciterElement & el) {
        if (!el.IsValid())
        {
            return;
        }
        if (globallyEnabled)
        {
            el.SetState(0, SciterElement::STATE_DISABLED, true);
        }
        else
        {
            el.SetState(SciterElement::STATE_DISABLED, 0, true);
        }
    };

    setDisabled(m_vibrationGroup);
    setDisabled(m_vibrationEnabled);
    setDisabled(m_vibrationStrength);
    setDisabled(m_vibrationStrengthLabel);
    setDisabled(m_vibrationStrengthPopupBtn);
    setDisabled(m_vibrationStrengthAnchor);
}

SciterElement InputConfigPlayer::GetControllerSvg()
{
    NpadStyleIndex layout = GetControllerTypeFromIndex(m_comboControllerType->CurrentIndex());
    switch (layout)
    {
    case NpadStyleIndex::Fullkey:
        return m_page.GetElementByID("ProController");
    case NpadStyleIndex::Handheld:
        return m_page.GetElementByID("Handheld");
    case NpadStyleIndex::JoyconDual:
        return m_page.GetElementByID("DualJoycons");
    case NpadStyleIndex::JoyconLeft:
        return m_page.GetElementByID("LeftJoycon");
    case NpadStyleIndex::JoyconRight:
        return m_page.GetElementByID("RightJoycon");
    case NpadStyleIndex::GameCube:
        return m_page.GetElementByID("GCController");
    }
    return SciterElement();
}

bool InputConfigPlayer::IsInputAcceptable(const IParamPackage & params) const
{
    if (m_comboDevices->CurrentIndex() == 0)
    {
        return true;
    }

    if (params.Has("motion"))
    {
        return true;
    }

    // Keyboard/Mouse
    if (m_comboDevices->CurrentIndex() == 1 || m_comboDevices->CurrentIndex() == 2)
    {
        return strcmp(params.GetString("engine", ""), "keyboard") == 0 || strcmp(params.GetString("engine", ""), "mouse") == 0;
    }

    const IParamPackage & device = m_inputDeviceList.GetParamPackage(m_comboDevices->CurrentIndex());
    return strcmp(params.GetString("engine", ""), device.GetString("engine", "")) == 0 && (strcmp(params.GetString("guid", ""), device.GetString("guid", "")) == 0 || strcmp(params.GetString("guid", ""), device.GetString("guid2", "")) == 0) && params.GetInt("port", 0) == device.GetInt("port", 0);
}

void InputConfigPlayer::PollingDone()
{
    if (m_pollingType == PollingInputType::Button && m_buttonMap[m_pollingButtonId].IsValid())
    {
        ParamPackage param(m_emulatedController->GetButtonParamPtr(m_pollingButtonId));
        m_buttonMap[m_pollingButtonId].SetText(ButtonToText(param).c_str());
    }
    m_operatingSystem.StopMapping();
    m_page.SetTimer(0, (uint32_t *)TIMER_TIMEOUT);
    m_page.SetTimer(0, (uint32_t*)TIMER_POLL);
    m_timeoutTimerActive = false;
    m_pollingType = PollingInputType::None;
    m_pollingButtonId = 0;
    UpdateButtonState();
    UpdateUI();
}

void InputConfigPlayer::stControllerEventCallback(ControllerTriggerType type, void * user)
{
    ((InputConfigPlayer*)user)->ControllerEventCallback(type);
}

void InputConfigPlayer::stVibrationSettingChanged(const char * /*setting*/, void * userData)
{
    InputConfigPlayer * player = static_cast<InputConfigPlayer *>(userData);
    if (player != nullptr && player->m_callbackEnabled)
    {
        player->UpdateVibrationControlsEnabled();
    }
}
