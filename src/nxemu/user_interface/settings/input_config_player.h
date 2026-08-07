#pragma once
#include <chrono>
#include <sciter_element.h>
#include <widgets/combo_box.h>
#include <nxemu-module-spec/operating_system.h>

class InputConfig;

class InputConfigPlayer :
    public IClickSink,
    public IStateChangeSink,
    public ITimerSink,
    public IKeySink,
    public IEventSink
{
    enum
    {
        TIMER_TIMEOUT = 200,
        TIMER_POLL = 300,
    };

public:
    InputConfigPlayer(ISciterUI& sciterUI, InputConfig & config, SystemModules & modules, HWINDOW parent, SciterElement page, NpadIdType controllerIndex);
    ~InputConfigPlayer();

    void SaveSetting();

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // IStateChangeSink
    bool OnStateChange(SCITER_ELEMENT elem, uint32_t eventReason, void * data) override;

    // ITimerSink
    bool OnTimer(SCITER_ELEMENT Element, uint32_t * TimerId) override;

    // IKeySink
    bool OnKeyDown(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyUp(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyChar(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;

    // IEventSink
    bool OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t event_code, uint64_t reason) override;

private:
    InputConfigPlayer() = delete;
    InputConfigPlayer(const InputConfigPlayer&) = delete;
    InputConfigPlayer& operator=(const InputConfigPlayer&) = delete;

    void BindControls();
    void UnbindControls();
    void LoadConfiguration();
    void UpdateInputDeviceCombobox();
    void UpdateInputDevices();
    void UpdateUI();
    std::string ButtonToText(const IParamPackage & param);
    std::string AnalogToText(const IParamPackage & param, const std::string& dir);
    int GetIndexFromControllerType(NpadStyleIndex type) const;
    NpadStyleIndex GetControllerTypeFromIndex(int index) const;
    void SetConnectableControllers();
    void ControllerTypeChanged();
    void UpdateMappingWithDefaults();
    void HandleClick(SciterElement& button, uint32_t button_id, PollingInputType type);
    void ControllerEventCallback(ControllerTriggerType type);
    void RefreshStickUi();
    void UpdateControllerAvailableButtons();
    void UpdateControllerGraphic(NpadStyleIndex layout);
    void UpdateControllerEnabledButtons();
    void UpdateControllerButtonNames();
    void UpdateButtonState();
    void UpdateMotionButtons();
    void UpdateMotionCube();
    void UpdateStickDisplay(const SciterElement & svg, NativeAnalogValues analog);
    void DeadzoneSliderChanged(uint32_t analogId);
    void LoadVibrationControls();
    void SaveVibrationControls();
    void UpdateVibrationStrengthDisplay();
    void UpdateVibrationControlsEnabled();
    const char * VibrationEnabledSettingId() const;
    const char * VibrationStrengthSettingId() const;
    SciterElement GetControllerSvg();
    bool IsInputAcceptable(const IParamPackage & params) const;
    void PollingDone();

    static void stControllerEventCallback(ControllerTriggerType type, void * user);
    static void stVibrationSettingChanged(const char * setting, void * userData);

    IOperatingSystem & m_operatingSystem;
    ISciterUI & m_sciterUI;
    InputConfig & m_config;
    SystemModules & m_modules;
    HWINDOW m_parent;
    SciterElement m_page;
    NpadIdType m_controllerIndex;
    IEmulatedController * m_emulatedController;
    IEmulatedController & m_emulatedControllerPlayer;
    IEmulatedController & m_emulatedControllerHandheld;
    SciterElement m_connectedController;
    SciterElement m_vibrationGroup;
    SciterElement m_vibrationEnabled;
    SciterElement m_vibrationStrength;
    SciterElement m_vibrationStrengthLabel;
    SciterElement m_vibrationStrengthAnchor;
    SciterElement m_vibrationStrengthPopupBtn;
    SciterElement m_vibrationStrengthPopup;
    SciterElement m_buttonMap[22];
    SciterElement m_motionMap[2];
    SciterElement m_motionGroup[2];
    SciterElement m_centerButtonsMotion;
    SciterElement m_analogMapButtons[2][4];
    SciterElement m_analogMapModifierButton[2];
    SciterElement m_analogMapDeadzoneLabel[2];
    SciterElement m_analogMapDeadzoneSlider[2];
    SciterElement m_analogMapModifierGroupbox[2];
    SciterElement m_analogMapModifierLabel[2];
    SciterElement m_analogMapModifierSlider[2];
    SciterElement m_analogMapRangeGroupbox[2];
    SciterElement m_analogMapRangeSpinbox[2];
    std::shared_ptr<IComboBox> m_comboControllerType;
    std::shared_ptr<IComboBox> m_comboDevices;
    const IParamPackageList & m_inputDeviceList;
    bool m_timeoutTimerActive;
    MotionState m_motionValues;
    SticksValues m_stickValues;
    button_status_t m_buttonValues[(int32_t)NativeButtonValues::NumButtons];
    PollingInputType m_pollingType;
    uint32_t m_pollingButtonId;
    std::vector<std::pair<int, NpadStyleIndex>> index_controller_type_pairs;
    std::chrono::steady_clock::time_point m_stickUiThrottleLast;
    std::chrono::steady_clock::time_point m_motionUiThrottleLast;
    NpadStyleIndex m_displayedControllerLayout;
    bool m_callbackEnabled;
};
