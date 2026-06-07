#pragma once
#include "base.h"
#include <stdint.h>

typedef struct CabinetParameters
{
    uint8_t tag_info[0x58];
    uint8_t register_info[0x100];
    uint8_t cabinet_mode;
} CabinetParameters;

typedef void (CALL *CabinetFinishedFn)(void * user_data, bool success, const char * amiibo_name_utf8);

nxinterface ICabinetFrontendApplet
{
    virtual void Close() = 0;
    virtual void ShowCabinetApplet(void * user_data, CabinetFinishedFn on_finished, const CabinetParameters * parameters) = 0;
};

typedef struct ControllerHostParameters
{
    int8_t min_players;
    int8_t max_players;
    bool keep_controllers_connected;
    bool enable_single_mode;
    bool enable_border_color;
    bool enable_explain_text;
    bool allow_pro_controller;
    bool allow_handheld;
    bool allow_dual_joycons;
    bool allow_left_joycon;
    bool allow_right_joycon;
    bool allow_gamecube_controller;
    uint32_t border_color_count;
    uint8_t border_colors[64][4];
    uint32_t explain_text_count;
    char explain_text[64][0x81];
} ControllerHostParameters;

typedef void (CALL *ControllerReconfigureFn)(void * user_data, bool ok);

nxinterface IControllerFrontendApplet
{
    virtual void Close() = 0;
    virtual void ReconfigureControllers(void * user_data, ControllerReconfigureFn on_complete, const ControllerHostParameters * parameters) = 0;
};

typedef void (CALL *SimpleFinishedFn)(void * user_data);

nxinterface IErrorFrontendApplet
{
    virtual void Close() = 0;
    virtual void ShowError(uint32_t result_raw, void * user_data, SimpleFinishedFn finished) const = 0;
    virtual void ShowErrorWithTimestamp(uint32_t result_raw, int64_t time_unix_seconds, void * user_data, SimpleFinishedFn finished) const = 0;
    virtual void ShowCustomErrorText(uint32_t result_raw, const char * dialog_text_utf8, const char * fullscreen_text_utf8, void * user_data, SimpleFinishedFn finished) const = 0;
};

nxinterface IMiiEditFrontendApplet
{
    virtual void Close() = 0;
    virtual void ShowMiiEdit(void * user_data, SimpleFinishedFn finished) const = 0;
};

typedef void (CALL *BoolFinishedFn)(void * user_data, bool ok);

nxinterface IParentalControlsFrontendApplet
{
    virtual void Close() = 0;
    virtual void VerifyPIN(void * user_data, BoolFinishedFn finished, bool suspend_future_verification_temporarily) = 0;
    virtual void VerifyPINForSettings(void * user_data, BoolFinishedFn finished) = 0;
    virtual void RegisterPIN(void * user_data, SimpleFinishedFn finished) = 0;
    virtual void ChangePIN(void * user_data, SimpleFinishedFn finished) = 0;
};

nxinterface IPhotoViewerFrontendApplet
{
    virtual void Close() = 0;
    virtual void ShowPhotosForApplication(uint64_t title_id, void * user_data, SimpleFinishedFn finished) const = 0;
    virtual void ShowAllPhotos(void * user_data, SimpleFinishedFn finished) const = 0;
};

enum class ProfileUiMode : uint32_t
{
    UserSelector = 0,
    UserCreator = 1,
    EnsureNetworkServiceAccountAvailable = 2,
    UserIconEditor = 3,
    UserNicknameEditor = 4,
    UserCreatorForStarter = 5,
    NintendoAccountAuthorizationRequestContext = 6,
    IntroduceExternalNetworkServiceAccount = 7,
    IntroduceExternalNetworkServiceAccountForRegistration = 8,
    NintendoAccountNnidLinker = 9,
    LicenseRequirementsForNetworkService = 10,
    LicenseRequirementsForNetworkServiceWithUserContextImpl = 11,
    UserCreatorForImmediateNaLoginTest = 12,
    UserQualificationPromoter = 13,
};

enum class UserSelectionPurposeHost : uint32_t
{
    General = 0,
    GameCardRegistration = 1,
    EShopLaunch = 2,
    EShopItemShow = 3,
    PicturePost = 4,
    NintendoAccountLinkage = 5,
    SettingsUpdate = 6,
    SaveDataDeletion = 7,
    UserMigration = 8,
    SaveDataTransfer = 9,
};

typedef struct ProfileDisplayHostOptions
{
    bool is_network_service_account_required;
    bool is_skip_enabled;
    bool is_system_or_launcher;
    bool is_registration_permitted;
    bool show_skip_button;
    bool additional_select;
    bool show_user_selector;
    bool is_unqualified_user_selectable;
} ProfileDisplayHostOptions;

typedef struct ProfileSelectHostParameters
{
    ProfileUiMode mode;
    uint8_t invalid_uid_list[8][16];
    ProfileDisplayHostOptions display_options;
    UserSelectionPurposeHost purpose;
} ProfileSelectHostParameters;

typedef void (CALL *ProfileSelectFinishedFn)(void * user_data, bool has_uuid, const uint8_t uuid_bytes[16]);

nxinterface IProfileSelectFrontendApplet
{
    virtual void Close() = 0;
    virtual void SelectProfile(void * user_data, ProfileSelectFinishedFn finished, const ProfileSelectHostParameters * parameters) const = 0;
};

enum class SwkbdTypeHost : uint32_t
{
    Normal = 0,
    NumberPad = 1,
    Qwerty = 2,
    Unknown3 = 3,
    Latin = 4,
    SimplifiedChinese = 5,
    TraditionalChinese = 6,
    Korean = 7,
};

enum class SwkbdPasswordModeHost : uint32_t
{
    Disabled = 0,
    Enabled = 1,
};

enum class SwkbdTextDrawTypeHost : uint32_t
{
    Line = 0,
    Box = 1,
    DownloadCode = 2,
};

enum class SwkbdResultHost : uint32_t
{
    Ok = 0,
    Cancel = 1,
};

enum class SwkbdTextCheckResultHost : uint32_t
{
    Success = 0,
    Failure = 1,
    Confirm = 2,
    Silent = 3,
};

enum class SwkbdReplyTypeHost : uint32_t
{
    FinishedInitialize = 0x0,
    Default = 0x1,
    ChangedString = 0x2,
    MovedCursor = 0x3,
    MovedTab = 0x4,
    DecidedEnter = 0x5,
    DecidedCancel = 0x6,
    ChangedStringUtf8 = 0x7,
    MovedCursorUtf8 = 0x8,
    DecidedEnterUtf8 = 0x9,
    UnsetCustomizeDic = 0xA,
    ReleasedUserWordInfo = 0xB,
    UnsetCustomizedDictionaries = 0xC,
    ChangedStringV2 = 0xD,
    MovedCursorV2 = 0xE,
    ChangedStringUtf8V2 = 0xF,
    MovedCursorUtf8V2 = 0x10,
};

typedef struct KeyboardInitializeParameters
{
    uint16_t ok_text[9];
    uint32_t ok_text_unit_count;
    uint16_t header_text[65];
    uint32_t header_text_unit_count;
    uint16_t sub_text[129];
    uint32_t sub_text_unit_count;
    uint16_t guide_text[257];
    uint32_t guide_text_unit_count;
    uint16_t initial_text[0x3EA];
    uint32_t initial_text_unit_count;
    uint16_t left_optional_symbol_key;
    uint16_t right_optional_symbol_key;
    uint32_t max_text_length;
    uint32_t min_text_length;
    int32_t initial_cursor_position;
    SwkbdTypeHost type;
    SwkbdPasswordModeHost password_mode;
    SwkbdTextDrawTypeHost text_draw_type;
    uint32_t key_disable_flags_raw;
    bool use_blur_background;
    bool enable_backspace_button;
    bool enable_return_button;
    bool disable_cancel_button;
} KeyboardInitializeHostParameters;

typedef struct InlineAppearParameters
{
    uint32_t max_text_length;
    uint32_t min_text_length;
    float key_top_scale_x;
    float key_top_scale_y;
    float key_top_translate_x;
    float key_top_translate_y;
    SwkbdTypeHost type;
    uint32_t key_disable_flags_raw;
    bool key_top_as_floating;
    bool enable_backspace_button;
    bool enable_return_button;
    bool disable_cancel_button;
} InlineAppearHostParameters;

typedef struct InlineTextHostParameters
{
    uint16_t input_text[0x3EA];
    uint32_t input_text_unit_count;
    int32_t cursor_position;
} InlineTextHostParameters;

typedef void (CALL *SwkbdSubmitNormalFn)(void * user_data, uint32_t result_raw, const uint16_t * text_utf16, uint32_t text_utf16_unit_count, bool confirmed);
typedef void (CALL *SwkbdSubmitInlineFn)(void * user_data, uint32_t reply_raw, const uint16_t * text_utf16, uint32_t text_utf16_unit_count, int32_t cursor);

nxinterface ISoftwareKeyboardFrontendApplet
{
    virtual void Close() = 0;
    virtual void InitializeKeyboard(bool is_inline, const KeyboardInitializeParameters * initialize_parameters, void * user_data_normal, SwkbdSubmitNormalFn submit_normal, void * user_data_inline, SwkbdSubmitInlineFn submit_inline) = 0;
    virtual void ShowNormalKeyboard() const = 0;
    virtual void ShowTextCheckDialog(uint32_t text_check_result_raw, const uint16_t * message_utf16, uint32_t message_utf16_unit_count) const = 0;
    virtual void ShowInlineKeyboard(const InlineAppearHostParameters * appear_parameters) const = 0;
    virtual void HideInlineKeyboard() const = 0;
    virtual void InlineTextChanged(const InlineTextHostParameters * text_parameters) const = 0;
    virtual void ExitKeyboard() const = 0;
};

enum class WebExitReasonHost : uint32_t
{
    EndButtonPressed = 0,
    BackButtonPressed = 1,
    ExitRequested = 2,
    CallbackURL = 3,
    WindowClosed = 4,
    ErrorDialog = 7,
};

typedef void (CALL *ExtractRomFsFn)(void * user_data);
typedef void (CALL *OpenWebPageFn)(void * user_data, uint32_t exit_reason_raw, const char * last_url_utf8);

nxinterface IWebBrowserFrontendApplet
{
    virtual void Close() = 0;
    virtual void OpenLocalWebPage(const char * local_url_utf8, void * extract_user_data, ExtractRomFsFn extract_romfs, void * open_user_data, OpenWebPageFn open_callback) const = 0;
    virtual void OpenExternalWebPage(const char * external_url_utf8, void * user_data, OpenWebPageFn callback) const = 0;
};

enum class StorageId : uint8_t 
{
    None = 0,
    Host = 1,
    GameCard = 2,
    NandSystem = 3,
    NandUser = 4,
    SdCard = 5,
};

// This is nn::hid::NpadIdType
enum class NpadIdType : uint32_t
{
    Player1 = 0x0,
    Player2 = 0x1,
    Player3 = 0x2,
    Player4 = 0x3,
    Player5 = 0x4,
    Player6 = 0x5,
    Player7 = 0x6,
    Player8 = 0x7,
    Other = 0x10,
    Handheld = 0x20,

    Invalid = 0xFFFFFFFF,
};

enum class NativeButtonValues : uint32_t
{
    A,
    B,
    X,
    Y,
    LStick,
    RStick,
    L,
    R,
    ZL,
    ZR,
    Plus,
    Minus,

    DLeft,
    DUp,
    DRight,
    DDown,

    SLLeft,
    SRLeft,

    Home,
    Screenshot,

    SLRight,
    SRRight,

    NumButtons,
};

enum class NativeMotionValues : uint32_t
{
    MotionLeft,
    MotionRight,

    NumMotions,
};

enum class NativeAnalogValues : uint32_t {
    LStick,
    RStick,

    NumAnalogs,
};

enum class ButtonNames : uint32_t
{
    Undefined,
    Invalid,
    // This will display the engine name instead of the button name
    Engine,
    // This will display the button by value instead of the button name
    Value,

    // Joycon button names
    ButtonLeft,
    ButtonRight,
    ButtonDown,
    ButtonUp,
    ButtonA,
    ButtonB,
    ButtonX,
    ButtonY,
    ButtonPlus,
    ButtonMinus,
    ButtonHome,
    ButtonCapture,
    ButtonStickL,
    ButtonStickR,
    TriggerL,
    TriggerZL,
    TriggerSL,
    TriggerR,
    TriggerZR,
    TriggerSR,

    // GC button names
    TriggerZ,
    ButtonStart,

    // DS4 button names
    L1,
    L2,
    L3,
    R1,
    R2,
    R3,
    Circle,
    Cross,
    Square,
    Triangle,
    Share,
    Options,
    Home,
    Touch,

    // Mouse buttons
    ButtonMouseWheel,
    ButtonBackward,
    ButtonForward,
    ButtonTask,
    ButtonExtra,
};

// This is nn::hid::NpadStyleIndex
enum class NpadStyleIndex : uint8_t {
    None = 0,
    Fullkey = 3,
    Handheld = 4,
    HandheldNES = 4,
    JoyconDual = 5,
    JoyconLeft = 6,
    JoyconRight = 7,
    GameCube = 8,
    Pokeball = 9,
    NES = 10,
    SNES = 12,
    N64 = 13,
    SegaGenesis = 14,
    SystemExt = 32,
    System = 33,
    MaxNpadType = 34,
};

// This is nn::hid::NpadStyleSet
enum class NpadStyleSet : uint32_t {
    None = 0,
    Fullkey = 1U << 0,
    Handheld = 1U << 1,
    JoyDual = 1U << 2,
    JoyLeft = 1U << 3,
    JoyRight = 1U << 4,
    Gc = 1U << 5,
    Palma = 1U << 6,
    Lark = 1U << 7,
    HandheldLark = 1U << 8,
    Lucia = 1U << 9,
    Lagoon = 1U << 10,
    Lager = 1U << 11,
    SystemExt = 1U << 29,
    System = 1U << 30,

    All = 0xFFFFFFFFU,
};

enum class PollingInputType 
{
    None, 
    Button, 
    Stick, 
    Motion, 
    Touch 
};

enum class ControllerTriggerType : uint32_t {
    Button,
    Stick,
    Trigger,
    Motion,
    Color,
    Battery,
    Vibration,
    IrSensor,
    RingController,
    Nfc,
    Connected,
    Disconnected,
    Type,
    All,
};

struct PerfStatsResults
{
    /// System FPS (LCD VBlanks) in Hz
    double system_fps;
    /// Average game FPS (GPU frame renders) in Hz
    double average_game_fps;
    /// Walltime per system frame, in seconds, excluding any waits
    double frametime;
    /// Ratio of walltime / emulated time elapsed
    double emulation_speed;
};

nxinterface IDeviceMemory
{
    virtual const uint8_t * BackingBasePointer() const = 0;
};

nxinterface ICacheInvalidator
{
    virtual void OnCacheInvalidation(uint64_t address, uint32_t size) = 0;
};

typedef void (*DeviceEnumCallback)(const char * device, void * userData);

nxinterface IParamPackage
{
    virtual bool Has(const char * key) const = 0;
    virtual bool GetBool(const char * key, bool default_value) const = 0;
    virtual int32_t GetInt(const char * key, int32_t default_value) const = 0;
    virtual float GetFloat(const char * key, float default_value) const = 0;
    virtual const char * GetString(const char * key, const char * default_value) const = 0;
    virtual void SetFloat(const char * key, float value) = 0;
    virtual const char * Serialize() const = 0;

    virtual void Release() = 0;
};

nxinterface IParamPackageList
{
    virtual uint32_t GetCount() const = 0;
    virtual IParamPackage & GetParamPackage(uint32_t index) const = 0;
    virtual void Release() = 0;
};

struct vec3f_t
{
    float x;
    float y;
    float z;
};

struct ControllerMotion
{
    vec3f_t accel;
    vec3f_t gyro;
    vec3f_t rotation;
    vec3f_t euler;
    vec3f_t orientation[3];
    bool atRest;
};

struct MotionState
{
    ControllerMotion motion[2];
};

// Analog properties for calibration
struct AnalogProperties 
{
    // Anything below this value will be detected as zero
    float deadzone;
    // Anything above this values will be detected as one
    float range;
    // Minimum value to be detected as active
    float threshold;
    // Drift correction applied to the raw data
    float offset;
    // Invert direction of the sensor data
    bool inverted;
    // Invert the state if it's converted to a button
    bool inverted_button;
    // Press once to activate, press again to release
    bool toggle;
};

// Single analog sensor data
struct AnalogStatus {
    float value;
    float raw_value;
    AnalogProperties properties;
};

// Analog and digital joystick data
struct StickStatus {
    unsigned char uuid[16];
    AnalogStatus x{};
    AnalogStatus y{};
    bool left;
    bool right;
    bool up;
    bool down;
};

struct SticksValues
{
    StickStatus status[(size_t)NativeAnalogValues::NumAnalogs];
};

typedef struct {
    unsigned char uuid[16];
    bool value;
    bool inverted;            // Invert value of the button
    bool toggle;              // Press once to activate, press again to release
    bool turbo;               // Spams the button when active
    bool locked;              // Internal lock for the toggle status
} button_status_t;

typedef void (CALL* ControllerEventCallback)(ControllerTriggerType type, void * user);

nxinterface IEmulatedController
{
    virtual void Connect(bool use_temporary_value = false) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected(bool get_temporary_value = false) const = 0;
    virtual void SaveCurrentConfig() = 0;
    virtual void ReloadFromSettings() = 0;
    virtual IParamPackageList * GetMappedDevicesPtr() const = 0;
    virtual IParamPackage * GetButtonParamPtr(uint32_t index) const = 0;
    virtual IParamPackage * GetMotionParamPtr(uint32_t index) const = 0;
    virtual IParamPackage * GetStickParamPtr(uint32_t index) const = 0;
    virtual NpadStyleIndex GetNpadStyleIndex(bool getTemporaryValue = false) const = 0;
    virtual void SetButtonParam(uint32_t index, const IParamPackage & param) = 0;
    virtual void SetStickParam(uint32_t index, const IParamPackage & param) = 0;
    virtual void SetMotionParam(uint32_t index, const IParamPackage & param) = 0;
    virtual void SetControllerEventCallback(ControllerEventCallback cb, void * user) = 0;
    virtual void GetButtonsStatus(button_status_t * buttons, size_t num_buttons) const = 0;
    virtual void SetNpadStyleIndex(NpadStyleIndex npad_type) = 0;
    virtual MotionState GetMotions() const = 0;
    virtual SticksValues GetSticksValues() const = 0;
};

nxinterface IButtonMappingList
{
    virtual uint32_t GetCount() const = 0;
    virtual uint32_t GetIndex(uint32_t position) const = 0;
    virtual IParamPackage& GetParamPackage(uint32_t position) const = 0;
    virtual void Release() = 0;
};

nxinterface IOperatingSystem
{
    virtual bool Initialize() = 0;
    virtual void ShutDown() = 0;
    virtual bool IsShuttingDown() const = 0;
    virtual bool IsPoweredOn() const = 0;
    virtual void ShutdownMainProcess() = 0;
    virtual bool CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl) = 0;
    virtual void StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen) = 0;
    virtual bool LoadModule(const IModuleInfo & module, uint64_t baseAddress) = 0;
    virtual IDeviceMemory & DeviceMemory() = 0;
    virtual void KeyboardKeyPress(int modifier, int keyIndex, int keyCode) = 0;
    virtual void KeyboardKeyRelease(int modifier, int keyIndex, int keyCode) = 0;
    virtual void GatherGPUDirtyMemory(ICacheInvalidator * invalidator) = 0;
    virtual uint64_t GetGPUTicks() = 0;
    virtual uint64_t GetProgramId() = 0;
    virtual bool GetExitLocked() const = 0;
    virtual void GameFrameEnd() = 0;
    virtual void AudioGetSyncIDs(uint32_t * ids, uint32_t maxCount, uint32_t * actualCount) = 0;
    virtual void AudioGetDeviceListForSink(uint32_t sinkId, bool capture, DeviceEnumCallback callback, void * userData) = 0;
    virtual void RegisterHostThread() = 0;
    virtual IParamPackageList * GetInputDevices() const = 0;
    virtual IEmulatedController & GetEmulatedController(NpadIdType index) = 0;
    virtual ButtonNames GetButtonName(const IParamPackage & param)  const = 0;
    virtual bool IsController(const IParamPackage & params)  const = 0;
    virtual NpadStyleSet GetSupportedStyleTag() const = 0;
    virtual IButtonMappingList * GetButtonMappingForDevice(const IParamPackage & param) const = 0;
    virtual IButtonMappingList * GetAnalogMappingForDevice(const IParamPackage & param) const = 0;
    virtual IButtonMappingList * GetMotionMappingForDevice(const IParamPackage & param) const = 0;
    virtual void BeginMapping(PollingInputType type) = 0;
    virtual void StopMapping() = 0;
    virtual IParamPackage * GetNextInput() const = 0;
    virtual void PumpInputEvents() const = 0;
    virtual PerfStatsResults GetAndResetPerfStats() = 0;
    virtual void SetEmulationPaused(bool paused) = 0;
    virtual bool IsEmulationPaused() const = 0;
    virtual void SetFrontendApplets(ICabinetFrontendApplet * cabinet, IControllerFrontendApplet * controller, IErrorFrontendApplet * error, IMiiEditFrontendApplet * mii_edit, IParentalControlsFrontendApplet * parental_controls, IPhotoViewerFrontendApplet * photo_viewer, IProfileSelectFrontendApplet * profile_select, ISoftwareKeyboardFrontendApplet * software_keyboard, IWebBrowserFrontendApplet * web_browser) = 0;
    virtual void SetPlayerButtonState(uint32_t player_index, uint32_t button_ordinal, bool pressed) = 0;
    virtual void SetPlayerAnalogState(uint32_t player_index, uint32_t stick_index, float x, float y) = 0;
};

EXPORT IOperatingSystem * CALL CreateOperatingSystem(ISystemModules & modules);
EXPORT void CALL DestroyOperatingSystem(IOperatingSystem * operatingSystem);
