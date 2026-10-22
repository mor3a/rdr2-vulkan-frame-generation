#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

// Stores only explicit failures reported by their owning subsystem. A successful
// retry must clear its matching code; configuration requests are not failures.
namespace RuntimeErrors {
enum class Code : unsigned {
    CameraMissing, CameraApiMissing, CameraStale, InputState, InputInvalid,
    TagsFailed, ConstantsFailed, ProviderInit, ProviderOptions, ProviderState,
    VulkanDevice, MenuInit, MenuRender, FsrRuntime, FsrFeatures, FsrPrepare,
    FsrPresent, HookInstall, PresentFailed, GenerationStalled, MenuInput, CursorHook, UnlockInit, MenuInputRecursion, FrameIndex, InputMissing, HardwareScheduling, Count
};
enum class Warning : std::uint32_t { VRAM=1, Telemetry=2 };
inline constexpr std::size_t Capacity=static_cast<std::size_t>(Code::Count);
inline bool Valid(Code c) { return static_cast<unsigned>(c)<Capacity; }
inline const char* CodeId(Code c) {
    constexpr const char* ids[]={"FG-101","FG-102","FG-103","FG-104","FG-105","FG-106","FG-107","FG-201","FG-202","FG-203","FG-204","UI-101","UI-102","FG-301","FG-302","FG-303","FG-304","SYS-101","SYS-102","FG-401","UI-103","UI-104","FG-205","UI-105","FG-108","FG-109","FG-206"};
    static_assert(sizeof(ids)/sizeof(*ids)==Capacity,"Every runtime code needs an ID");
    return Valid(c)?ids[static_cast<unsigned>(c)]:"UNKNOWN";
}
inline const char* Title(Code c) {
    constexpr const char* titles[]={"Camera connection unavailable","Camera API unavailable","Camera data expired","DLSS resource state unavailable","Invalid DLSS inputs","Resource tagging failed","Frame constants rejected","Provider initialization failed","Provider options rejected","Provider state query failed","Vulkan device setup failed","Menu initialization failed","Menu rendering failed","FSR runtime unavailable","FSR Vulkan features unavailable","FSR preparation failed","FSR presentation failed","Hook installation failed","Presentation failed","Frame generation stalled","Menu input fallback active","Menu cursor hooks unavailable","MFG core initialization failed","Nested menu input callback bypassed","Frame index mismatch","DLSS inputs not detected","Windows GPU scheduling required"};
    static_assert(sizeof(titles)/sizeof(*titles)==Capacity,"Every runtime code needs a title");
    return Valid(c)?titles[static_cast<unsigned>(c)]:"Unknown error";
}
inline const char* Description(Code c) {
    switch(c) {
    case Code::CameraMissing: return "ScriptHookRDR2 was not detected. Frame generation is paused.";
    case Code::CameraApiMissing: return "The ScriptHookRDR2 camera API is unavailable. Frame generation is paused.";
    case Code::CameraStale: return "Camera data stopped updating. Frame generation is paused until it resumes.";
    case Code::InputState: return "Depth or motion-vector resource states could not be verified. Frame generation is paused.";
    case Code::InputInvalid: return "The game's DLSS inputs are incomplete. Frame generation is paused.";
    case Code::TagsFailed: return "Depth or motion-vector resources were rejected. Frame generation is paused.";
    case Code::ConstantsFailed: return "Frame camera data was rejected. Frame generation is paused.";
    case Code::ProviderInit: return "The selected provider could not initialize. Frame generation is disabled for this session.";
    case Code::ProviderOptions: return "The provider rejected the requested settings. Frame generation is unavailable.";
    case Code::ProviderState: return "The provider reported a state problem. Displayed FPS may be unverified.";
    case Code::VulkanDevice: return "A Vulkan device problem was reported. Frame generation may be unavailable.";
    case Code::MenuInit: return "The menu could not initialize. Game rendering continues.";
    case Code::MenuRender: return "The menu overlay reported a rendering problem. Game rendering continues.";
    case Code::FsrRuntime: return "The FSR runtime could not be loaded. Frame generation is disabled.";
    case Code::FsrFeatures: return "Required Vulkan features are unavailable. Frame generation is disabled.";
    case Code::FsrPrepare: return "FSR frame preparation failed. Frame generation is paused.";
    case Code::FsrPresent: return "FSR presentation failed. Frame generation is paused.";
    case Code::HookInstall: return "Hook installation failed. Frame generation is disabled for this session.";
    case Code::PresentFailed: return "Presentation reported a failure. See the native result in the details.";
    case Code::GenerationStalled: return "Presentation stopped advancing for several seconds.";
    case Code::MenuInput: return "The message hook is unavailable. The menu uses polled keyboard and mouse input.";
    case Code::CursorHook: return "Cursor routing could not be installed safely. Menu controls may be restricted by the game.";
    case Code::UnlockInit: return "The bundled MFG core could not initialize. Native frame generation remains available on RTX 40.";
    case Code::MenuInputRecursion: return "A nested menu input callback was bypassed without changing the game's window procedure.";
    case Code::FrameIndex: return "NVIDIA frame token, DLSS inputs or presentation indices did not match. The mismatched input was rejected.";
    case Code::InputMissing: return "No DLSS input frames were captured during rendering. Frame generation cannot start.";
    case Code::HardwareScheduling: return "Streamline reports that Windows hardware-accelerated GPU scheduling (HAGS) is unavailable. DLSS-G cannot start.";
    default: return "An error was reported. See the details below.";
    }
}
inline const char* Action(Code c) {
    switch(c) {
    case Code::CameraMissing: return "Check that ScriptHookRDR2 is loaded. Camera registration will retry.";
    case Code::CameraApiMissing: return "Check ScriptHookRDR2 compatibility and include its log in your report.";
    case Code::CameraStale: return "Return to gameplay. Check the log if camera updates do not resume.";
    case Code::InputState: return "Return to gameplay with DLSS enabled. Report the log if this persists.";
    case Code::InputInvalid: return "Check that DLSS is enabled. Include the log if valid inputs do not resume.";
    case Code::TagsFailed: case Code::ConstantsFailed: return "Include this code and RDR2VulkanFG.log in your report.";
    case Code::ProviderInit: return "Check the mod runtime files and perform a clean installation.";
    case Code::ProviderOptions: case Code::ProviderState: return "Check provider details in the log. Include this code in your report.";
    case Code::VulkanDevice: return "Check Vulkan compatibility and include the log in your report.";
    case Code::MenuInit: case Code::MenuRender: return "Menu rendering will retry where possible. Include the log in your report.";
    case Code::FsrRuntime: return "Check the FSR runtime files and perform a clean installation.";
    case Code::FsrFeatures: return "Check the logged Vulkan feature requirements and driver compatibility.";
    case Code::FsrPrepare: case Code::FsrPresent: return "Include the native result and RDR2VulkanFG.log in your report.";
    case Code::HookInstall: return "Restart after checking for conflicting mods. Include the log in your report.";
    case Code::PresentFailed: return "Include the native Vulkan result and RDR2VulkanFG.log in your report.";
    case Code::GenerationStalled: return "Presentation stopped advancing. Include RDR2VulkanFG.log in your report.";
    case Code::MenuInput: return "Use the menu shortcut normally. Include this code and log if input is restricted.";
    case Code::CursorHook: return "Check other overlays or input mods. Include this code and the native Windows result.";
    case Code::UnlockInit: return "Check the bundled MFG runtime files. Include FG-205 and the log in your report.";
    case Code::MenuInputRecursion: return "Include this code and the log if other overlays or input hooks are active.";
    case Code::FrameIndex: return "Include FG-108 and RDR2VulkanFG.log. Frame index details are recorded automatically.";
    case Code::InputMissing: return "Enable the game's DLSS setting. If it is already enabled, check for another upscaling hook such as OptiScaler and include the log in your report.";
    case Code::HardwareScheduling: return "Enable Hardware-accelerated GPU scheduling in Windows Settings > System > Display > Graphics, then restart Windows.";
    default: return "Include the log in your report.";
    }
}
inline bool AffectsGeneration(Code code) {
    return code!=Code::MenuInit && code!=Code::MenuRender && code!=Code::MenuInput && code!=Code::CursorHook && code!=Code::UnlockInit && code!=Code::MenuInputRecursion;
}
struct Entry {
    Code code{};
    const char* id="";
    const char* title="";
    const char* action="";
    std::array<char,256> detail{};
    std::uint64_t count=0;
    std::uint64_t time=0; // steady-clock milliseconds, not wall time
    std::uint64_t firstTime=0; // first occurrence since the error last cleared
    int native=0;
};
struct Snapshot { std::array<Entry,Capacity> entries{}; std::size_t count=0; };
namespace Detail {
inline std::mutex mutex;
inline std::array<Entry,Capacity> entries{};
inline std::array<bool,Capacity> active{};

inline std::atomic<std::uint32_t> warnings{0};
}
inline void ReportError(Code code,const char* detail,int native=0) {
    if(!Valid(code)) return;
    std::lock_guard<std::mutex> lock(Detail::mutex);
    const auto index=static_cast<std::size_t>(code);
    auto& e=Detail::entries[index];
    if(!Detail::active[index]) e=Entry{};
    e.code=code; e.id=CodeId(code); e.title=Title(code); e.action=Action(code);
    e.native=native; ++e.count;
    e.time=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(e.count==1) e.firstTime=e.time;
    std::size_t n=0;
    if(detail) while(n+1<e.detail.size() && detail[n]) {e.detail[n]=detail[n]; ++n;}
    e.detail[n]='\0';
    Detail::active[index]=true;
}
inline void ClearError(Code code) {
    if(!Valid(code)) return;
    std::lock_guard<std::mutex> lock(Detail::mutex);
    Detail::active[static_cast<std::size_t>(code)]=false;
}
inline Snapshot SnapshotErrors() {
    std::lock_guard<std::mutex> lock(Detail::mutex);
    Snapshot out;
    for(std::size_t i=0;i<Capacity;++i) if(Detail::active[i]) out.entries[out.count++]=Detail::entries[i];
    return out;
}
inline void SetWarning(Warning warning,bool enabled) {
    const auto bit=static_cast<std::uint32_t>(warning);
    if(enabled) Detail::warnings.fetch_or(bit,std::memory_order_relaxed);
    else Detail::warnings.fetch_and(~bit,std::memory_order_relaxed);
}
inline std::uint32_t GetWarnings() { return Detail::warnings.load(std::memory_order_relaxed); }
}
