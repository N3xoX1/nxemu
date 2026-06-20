#pragma once
#include <stdint.h>
#include <string>

#if !defined(EXPORT) && defined(__cplusplus) && defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#elif !defined(EXPORT) && defined(__cplusplus)
#define EXPORT extern "C" __attribute__((visibility("default")))
#elif !defined(EXPORT) && defined(_WIN32)
#define EXPORT __declspec(dllexport)
#elif !defined(EXPORT)
#define EXPORT __attribute__((visibility("default")))
#endif

#ifndef CALL
#define CALL __cdecl
#else
#define CALL
#endif

#ifdef _MSC_VER
#define nxinterface __interface
#else
#define nxinterface struct
#endif

enum
{
    MODULE_LOADER_SPECS_VERSION = 0x0123,
    MODULE_VIDEO_SPECS_VERSION = 0x011A,
    MODULE_CPU_SPECS_VERSION = 0x0110,
    MODULE_OPERATING_SYSTEM_SPECS_VERSION = 0x0118,
};

enum MODULE_TYPE : uint16_t
{
    MODULE_TYPE_LOADER = 1,
    MODULE_TYPE_VIDEO = 2,
    MODULE_TYPE_CPU = 3,
    MODULE_TYPE_OPERATING_SYSTEM = 4,
};

enum class NotificationResponse : uint32_t
{
    No = 0,
    Yes = 1,
};

nxinterface IModuleNotification
{
    virtual void DisplayError(const char * message, const char * title) = 0;
    virtual void BreakPoint(const char * fileName, uint32_t lineNumber) = 0;
    virtual NotificationResponse Query(const char * message, const char * title) = 0;
};

typedef void (*SettingChangeCallback)(const char * setting, void * userData);

nxinterface IModuleSettings
{
    virtual const char * GetString(const char * setting) const = 0;
    virtual bool GetBool(const char * setting) const = 0;
    virtual int32_t GetInt(const char * setting) const = 0;
    virtual float GetFloat(const char * setting) const = 0;
    virtual void SetString(const char * setting, const char * value) = 0;
    virtual void SetBool(const char * setting, bool value) = 0;
    virtual void SetInt(const char * setting, int32_t value) = 0;
    virtual void SetFloat(const char * setting, float value) = 0;

    virtual void SetDefaultBool(const char * setting, bool value) = 0;
    virtual void SetDefaultInt(const char * setting, int value) = 0;
    virtual void SetDefaultFloat(const char * setting, float value) = 0;
    virtual void SetDefaultString(const char * setting, const char * value) = 0;

    virtual const char * GetSectionSettings(const char * section) const = 0;
    virtual void SetSectionSettings(const char * section, const std::string & json) = 0;

    virtual void RegisterCallback(const char * setting, SettingChangeCallback callback, void * userData) = 0;
    virtual void UnregisterCallback(const char * setting, SettingChangeCallback callback, void * userData) = 0;
};

/// Specifies the severity or level of detail of the log message.
enum class LogLevel : uint8_t 
{
    Trace,    ///< Extremely detailed and repetitive debugging information that is likely to
    ///< pollute logs.
    Debug,    ///< Less detailed debugging information.
    Info,     ///< Status information from important points during execution.
    Warning,  ///< Minor or potential problems found during execution of a task.
    Error,    ///< Major problems found during execution of a task that prevent it from being
    ///< completed.
    Critical, ///< Major problems during execution that threaten the stability of the entire
    ///< application.

    Count ///< Total number of logging levels
};

/**
 * Specifies the sub-system that generated the log message.
 *
 * @note If you add a new entry here, also add a corresponding one to `ALL_LOG_CLASSES` in
 * filter.cpp.
 */
enum class LogClass : uint8_t
{
    Log,                ///< Messages about the log system itself
    Common,             ///< Library routines
    Common_Filesystem,  ///< Filesystem interface library
    Common_Memory,      ///< Memory mapping and management functions
    Core,               ///< LLE emulation core
    Core_ARM,           ///< ARM CPU core
    Core_Timing,        ///< CoreTiming functions
    Config,             ///< Emulator configuration (including commandline)
    Debug,              ///< Debugging tools
    Debug_Emulated,     ///< Debug messages from the emulated programs
    Debug_GPU,          ///< GPU debugging tools
    Debug_Breakpoint,   ///< Logging breakpoints and watchpoints
    Debug_GDBStub,      ///< GDB Stub
    Kernel,             ///< The HLE implementation of the CTR kernel
    Kernel_SVC,         ///< Kernel system calls
    Service,            ///< HLE implementation of system services. Each major service
    ///< should have its own subclass.
    Service_ACC,        ///< The ACC (Accounts) service
    Service_AM,         ///< The AM (Applet manager) service
    Service_AOC,        ///< The AOC (AddOn Content) service
    Service_APM,        ///< The APM (Performance) service
    Service_ARP,        ///< The ARP service
    Service_Audio,      ///< The Audio (Audio control) service
    Service_BCAT,       ///< The BCAT service
    Service_BGTC,       ///< The BGTC (Background Task Controller) service
    Service_BPC,        ///< The BPC service
    Service_BTDRV,      ///< The Bluetooth driver service
    Service_BTM,        ///< The BTM service
    Service_Capture,    ///< The capture service
    Service_ERPT,       ///< The error reporting service
    Service_ETicket,    ///< The ETicket service
    Service_EUPLD,      ///< The error upload service
    Service_Fatal,      ///< The Fatal service
    Service_FGM,        ///< The FGM service
    Service_Friend,     ///< The friend service
    Service_FS,         ///< The FS (Filesystem) service
    Service_GRC,        ///< The game recording service
    Service_HID,        ///< The HID (Human interface device) service
    Service_IRS,        ///< The IRS service
    Service_JIT,        ///< The JIT service
    Service_LBL,        ///< The LBL (LCD backlight) service
    Service_LDN,        ///< The LDN (Local domain network) service
    Service_LDR,        ///< The loader service
    Service_LM,         ///< The LM (Logger) service
    Service_Migration,  ///< The migration service
    Service_Mii,        ///< The Mii service
    Service_MM,         ///< The MM (Multimedia) service
    Service_MNPP,       ///< The MNPP service
    Service_NCM,        ///< The NCM service
    Service_NFC,        ///< The NFC (Near-field communication) service
    Service_NFP,        ///< The NFP service
    Service_NGC,        ///< The NGC (No Good Content) service
    Service_NIFM,       ///< The NIFM (Network interface) service
    Service_NIM,        ///< The NIM service
    Service_NOTIF,      ///< The NOTIF (Notification) service
    Service_NPNS,       ///< The NPNS service
    Service_NS,         ///< The NS services
    Service_NVDRV,      ///< The NVDRV (Nvidia driver) service
    Service_Nvnflinger, ///< The Nvnflinger service
    Service_OLSC,       ///< The OLSC service
    Service_PCIE,       ///< The PCIe service
    Service_PCTL,       ///< The PCTL (Parental control) service
    Service_PCV,        ///< The PCV service
    Service_PM,         ///< The PM service
    Service_PREPO,      ///< The PREPO (Play report) service
    Service_PSC,        ///< The PSC service
    Service_PTM,        ///< The PTM service
    Service_SET,        ///< The SET (Settings) service
    Service_SM,         ///< The SM (Service manager) service
    Service_SPL,        ///< The SPL service
    Service_SSL,        ///< The SSL service
    Service_TCAP,       ///< The TCAP service.
    Service_Time,       ///< The time service
    Service_USB,        ///< The USB (Universal Serial Bus) service
    Service_VI,         ///< The VI (Video interface) service
    Service_WLAN,       ///< The WLAN (Wireless local area network) service
    HW,                 ///< Low-level hardware emulation
    HW_Memory,          ///< Memory-map and address translation
    HW_LCD,             ///< LCD register emulation
    HW_GPU,             ///< GPU control emulation
    HW_AES,             ///< AES engine emulation
    IPC,                ///< IPC interface
    Frontend,           ///< Emulator UI
    Render,             ///< Emulator video output and hardware acceleration
    Render_Software,    ///< Software renderer backend
    Render_OpenGL,      ///< OpenGL backend
    Render_Vulkan,      ///< Vulkan backend
    Shader,             ///< Shader recompiler
    Shader_SPIRV,       ///< Shader SPIR-V code generation
    Shader_GLASM,       ///< Shader GLASM code generation
    Shader_GLSL,        ///< Shader GLSL code generation
    Audio,              ///< Audio emulation
    Audio_DSP,          ///< The HLE implementation of the DSP
    Audio_Sink,         ///< Emulator audio output backend
    Loader,             ///< ROM loader
    CheatEngine,        ///< Memory manipulation and engine VM functions
    Input,              ///< Input emulation
    Network,            ///< Network emulation
    WebService,         ///< Interface to yuzu Web Services
    Count               ///< Total number of logging classes
};

nxinterface IModuleLogger
{
    virtual void Log(LogClass log_class, LogLevel log_level, const char * filename, unsigned int line_num, const char * function, const char * message) = 0;
};

typedef struct
{
    IModuleNotification * notification;
    IModuleSettings * settings;
    IModuleLogger * logger;
    void * java_vm;
    void * native_library_class;
} ModuleInterfaces;

typedef struct
{
    uint16_t version; // Should be set module spec version eg MODULE_VIDEO_SPECS_VERSION
    uint16_t type;    // Set to the module type, eg MODULE_TYPE_VIDEO
    char name[200];   // Name of the DLL
} MODULE_INFO;

nxinterface IRenderWindow
{
    virtual void * RenderSurface(void) const = 0;
    virtual float PixelRatio(void) const = 0;
};

nxinterface IOperatingSystem;
nxinterface IVideo;
nxinterface ICpu;
nxinterface ISystemloader;

enum class ProgramAddressSpaceType : uint8_t
{
    Is32Bit = 0,
    Is36Bit = 1,
    Is32BitNoMap = 2,
    Is39Bit = 3,
};

enum class PoolPartition : uint32_t
{
    Application = 0,
    Applet = 1,
    System = 2,
    SystemNonSecure = 3,
};

nxinterface IProgramMetadata
{
    virtual bool Is64BitProgram() const = 0;
    virtual ProgramAddressSpaceType GetAddressSpaceType() const = 0;
    virtual uint8_t GetMainThreadPriority() const = 0;
    virtual uint8_t GetMainThreadCore() const = 0;
    virtual uint32_t GetMainThreadStackSize() const = 0;
    virtual uint64_t GetTitleID() const = 0;
    virtual uint64_t GetFilesystemPermissions() const = 0;
    virtual uint32_t GetSystemResourceSize() const = 0;
    virtual PoolPartition GetPoolPartition() const = 0;
    virtual const uint32_t * GetKernelCapabilities() const = 0;
    virtual uint32_t GetKernelCapabilitiesSize() const = 0;
    virtual const char * GetName() const = 0;
};

nxinterface IModuleInfo
{
    virtual const uint8_t * Data(void) const = 0;
    virtual uint32_t DataSize(void) const = 0;
    virtual uint64_t CodeSegmentAddr(void) const = 0;
    virtual uint64_t CodeSegmentOffset(void) const = 0;
    virtual uint64_t CodeSegmentSize(void) const = 0;
    virtual uint64_t RODataSegmentAddr(void) const = 0;
    virtual uint64_t RODataSegmentOffset(void) const = 0;
    virtual uint64_t RODataSegmentSize(void) const = 0;
    virtual uint64_t DataSegmentAddr(void) const = 0;
    virtual uint64_t DataSegmentOffset(void) const = 0;
    virtual uint64_t DataSegmentSize(void) const = 0;
};

nxinterface IProcess
{
    virtual bool LoadFromMetadata(const IProgramMetadata & metadata, uint64_t code_size, bool is_hbl) = 0;
    virtual bool LoadModule(const IModuleInfo & module, uint64_t base_address) = 0;
    virtual uint64_t GetEntryPoint() const = 0;
    virtual uint64_t GetProcessId() const = 0;
};

nxinterface ISystemModules
{
    virtual void StartEmulation() = 0;
    virtual void StopEmulation(bool wait) = 0;

    virtual ISystemloader & Systemloader() = 0;
    virtual IOperatingSystem & OperatingSystem() = 0;
    virtual IVideo & Video() = 0;
    virtual ICpu & Cpu() = 0;
};

/*
Function: GetModuleInfo
Purpose: Fills the MODULE_INFO structure with information about the DLL.
Input: A pointer to a MODULE_INFO structure to be populated.
Output: none
*/
EXPORT void CALL GetModuleInfo(MODULE_INFO * info);

/*
Function: ModuleInitialize
Purpose: Initializes the module for global use.
Input: None
Output: Returns 0 on success
*/
EXPORT int CALL ModuleInitialize(ModuleInterfaces & interfaces);

/*
Function: ModuleCleanup
Purpose: Cleans up global resources used by the module.
Input: None
Output: None
*/
EXPORT void CALL ModuleCleanup();

/*
Function: EmulationStarting
Purpose: Called when emulation is starting
Input: None.
Output: None.
*/
EXPORT void CALL EmulationStarting();

/*
Function: EmulationStopping
Purpose: Called when emulation is stopping
Input: None
Output: None
*/
EXPORT void CALL EmulationStopping(bool wait);

/*
Function: FlushSettings
Purpose: Called when emulation is saving settings
Input: None
Output: None
*/
EXPORT void CALL FlushSettings();