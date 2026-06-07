#pragma once
#include <nxemu-module-spec/operating_system.h>
#include "core/core.h"
#include "emu_thread.h"

class OSManager :
    public IOperatingSystem
{
public:
    OSManager(ISystemModules & modules);
    ~OSManager();

    void EmulationStarting();
    void EmulationStopping(bool wait);

    // IOperatingSystem
    bool Initialize() override;
    void ShutDown() override;
    bool IsShuttingDown() const override;
    bool IsPoweredOn() const override;
    void ShutdownMainProcess() override;
    bool CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl) override;
    void StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen) override;
    bool LoadModule(const IModuleInfo & module, uint64_t baseAddress) override;
    IDeviceMemory & DeviceMemory() override;
    void KeyboardKeyPress(int modifier, int keyIndex, int keyCode) override;
    void KeyboardKeyRelease(int modifier, int keyIndex, int keyCode) override;
    void GatherGPUDirtyMemory(ICacheInvalidator * invalidator) override;
    uint64_t GetGPUTicks() override;
    uint64_t GetProgramId() override;
    bool GetExitLocked() const override;
    void GameFrameEnd() override;
    void AudioGetSyncIDs(uint32_t* ids, uint32_t maxCount, uint32_t* actualCount) override;
    void AudioGetDeviceListForSink(uint32_t sinkId, bool capture, DeviceEnumCallback callback, void* userData) override;
    void RegisterHostThread() override;
    IParamPackageList * GetInputDevices() const override;
    IEmulatedController & GetEmulatedController(NpadIdType index) override;
    ButtonNames GetButtonName(const IParamPackage & param) const override;
    bool IsController(const IParamPackage& params) const override;
    NpadStyleSet GetSupportedStyleTag() const override;
    IButtonMappingList * GetButtonMappingForDevice(const IParamPackage& param) const override;
    IButtonMappingList * GetAnalogMappingForDevice(const IParamPackage& param) const override;
    IButtonMappingList * GetMotionMappingForDevice(const IParamPackage& param) const override;
    void BeginMapping(PollingInputType type) override;
    void StopMapping() override;
    IParamPackage * GetNextInput() const override;
    void PumpInputEvents() const override;
    PerfStatsResults GetAndResetPerfStats() override;
    void SetEmulationPaused(bool paused) override;
    bool IsEmulationPaused() const override;
    void SetFrontendApplets(ICabinetFrontendApplet * cabinet, IControllerFrontendApplet * controller, IErrorFrontendApplet * error, IMiiEditFrontendApplet * mii_edit, IParentalControlsFrontendApplet * parental_controls, IPhotoViewerFrontendApplet * photo_viewer, IProfileSelectFrontendApplet * profile_select, ISoftwareKeyboardFrontendApplet * software_keyboard, IWebBrowserFrontendApplet * web_browser) override;
    void SetPlayerButtonState(uint32_t player_index, uint32_t button_ordinal, bool pressed) override;
    void SetPlayerAnalogState(uint32_t player_index, uint32_t stick_index, float x, float y) override;

private:
    OSManager() = delete;
    OSManager(const OSManager &) = delete;
    OSManager & operator=(const OSManager &) = delete;

    Core::System m_coreSystem;
    ISystemModules & m_modules;
    Kernel::KProcess * m_process;
    std::unique_ptr<EmuThread> m_emuThread;
};
