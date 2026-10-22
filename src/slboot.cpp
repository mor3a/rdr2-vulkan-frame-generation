#include "log.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "slboot.h"
#include "menu.h"
#include "vkhooks.h"
#include "config.h"
#include "performance_probe.h"
#include "latency_imports.h"
#include "runtime_errors.h"
#include "ampere_boot.h"

#include <sl.h>
#include <sl_helpers_vk.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_consts.h>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>
#include <mutex>
#include <detours.h>
#include <intrin.h>
#include <sl_core_api.h>

namespace fgvk {
static HMODULE g_sl{};
static wchar_t s_slDir[MAX_PATH]{};
// Streamline production builds consult this DRS setting even when OTA flags are
// disabled. Restrict the override to SL's caller and this process: no DRS writes.
using DrsGetSetting=int (__cdecl*)(void*,void*,unsigned int,void*);
static DrsGetSetting originalDrsGetSetting{};
static int __cdecl LocalDrsGetSetting(void* session,void* profile,unsigned int id,void* setting){
  if(id==0x10e41e06){
    HMODULE caller{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCSTR)_ReturnAddress(),&caller);
    if(caller==g_sl){
      static bool logged=false;if(!logged){logged=true;Log("Streamline profile override suppressed for this process; pinned local plugins selected");}
      return -160; // NVAPI_SETTING_NOT_FOUND; leave the caller's structure untouched.
    }
  }
  return originalDrsGetSetting(session,profile,id,setting);
}
static bool PinLocalPlugins(){
  if(originalDrsGetSetting)return true;
  HMODULE nvapi=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!nvapi){Log("Cannot pin local plugins: native nvapi64.dll unavailable");return false;}
  using QueryInterface=void* (__cdecl*)(unsigned int);
  auto query=(QueryInterface)GetProcAddress(nvapi,"nvapi_QueryInterface");
  if(!query)return false;
  originalDrsGetSetting=(DrsGetSetting)query(0x73bf8338);
  if(!originalDrsGetSetting)return false;
  if(DetourTransactionBegin()!=NO_ERROR){originalDrsGetSetting=nullptr;return false;}
  DetourUpdateThread(GetCurrentThread());
  LONG attach=DetourAttach(&(PVOID&)originalDrsGetSetting,(PVOID)LocalDrsGetSetting);
  if(attach!=NO_ERROR){DetourTransactionAbort();originalDrsGetSetting=nullptr;return false;}
  LONG result=DetourTransactionCommit();
  if(result!=NO_ERROR)originalDrsGetSetting=nullptr;
  Log("Process-only Streamline plugin pin commit=%ld",result);return result==NO_ERROR;
}
static bool LocalPluginLoaded(const wchar_t* name){
  HMODULE plugin=GetModuleHandleW(name);wchar_t path[MAX_PATH]{};
  if(!plugin)return false;
  GetModuleFileNameW(plugin,path,MAX_PATH);
  wchar_t expected[MAX_PATH]{};swprintf(expected,MAX_PATH,L"%s\\%s",s_slDir,name);
  return _wcsicmp(path,expected)==0;
}
static bool BindBundledLatency(){
  wchar_t path[MAX_PATH]{};swprintf(path,MAX_PATH,L"%s\\NvLowLatencyVk.dll",s_slDir);
  HMODULE latency=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  wchar_t loaded[MAX_PATH]{};if(latency)GetModuleFileNameW(latency,loaded,MAX_PATH);
  if(!latency || _wcsicmp(path,loaded) || !GetProcAddress(latency,"NvLL_VK_NotifyOutOfBandQueue")){
    Log("Bundled latency binding refused: expected=%S loaded=%S error=%lu",path,loaded,GetLastError());return false;
  }
  unsigned total=0;
  for(auto name:{L"sl.common.dll",L"sl.reflex.dll",L"sl.dlss_g.dll",L"sl.pcl.dll"}){
    LatencyBinding binding{};
    if(!BindLatencyImports(GetModuleHandleW(name),latency,binding)){
      Log("Bundled latency binding failed: plugin=%S reason=%s",name,binding.error?binding.error:"unknown");return false;
    }
    total+=binding.imports;Log("Bundled latency binding: plugin=%S imports=%u",name,binding.imports);
  }
  if(!total){Log("Bundled latency binding refused: no matching delay imports");return false;}
  Log("Streamline latency imports pinned to %S; imports=%u",loaded,total);return true;
}
static SlFns g_slFns;
SlFns& GetSlFns(){ return g_slFns; }



// --- Streamline function pointers, resolved dynamically from sl.interposer.dll ---
// No SL import lib is linked (see CMakeLists: only detours.lib). Every SL entry
// point below is pulled via GetProcAddress instead of calling the header's
// extern "C" declarations directly, so nothing here forces the linker to resolve
// an SL symbol.
static PFun_slInit* p_slInit{};
static PFun_slSetVulkanInfo* p_slSetVulkanInfo{};
static PFun_slGetFeatureFunction* p_slGetFeatureFunction{};
static PFun_slReflexSetOptions* p_slReflexSetOptions{};
static PFun_slDLSSGSetOptions* p_slDLSSGSetOptions{};
static PFun_slDLSSGGetState* p_slDLSSGGetState{};

// The Streamline runtime ships in a 'Streamline' folder next to fgvk.dll. Loading the interposer
// from there by full path makes Streamline search that folder for its plugins, nvngx_dlssg.dll
// and NvLowLatencyVk.dll (its default plugin path is the interposer's own directory; we also pass
// it explicitly). Fallback: the seven files next to RDR2.exe. ONE loader for every caller - the
// first release build resolved the proxy functions through a plain LoadLibraryA("sl.interposer.dll")
// before this ran, found nothing next to RDR2.exe, and the game failed to create its instance.
static HMODULE LoadInterposer(){
  if(g_sl) return g_sl;
  if(GetModuleHandleA("sl.interposer.dll")){Log("Another interposer is loaded; refusing mixed runtime");RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Another Streamline interposer is already loaded; mixed runtime refused");return nullptr;}
  HMODULE self{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&LoadInterposer,&self);
  wchar_t dir[MAX_PATH]{}; GetModuleFileNameW(self, dir, MAX_PATH); wchar_t* s=wcsrchr(dir,L'\\'); if(s) *(s+1)=0;
  wchar_t interposer[MAX_PATH]; swprintf(interposer, MAX_PATH, L"%sRDR2VulkanFG\\Streamline\\sl.interposer.dll", dir);
  if(GetFileAttributesW(interposer)!=INVALID_FILE_ATTRIBUTES){
    swprintf(s_slDir, MAX_PATH, L"%sRDR2VulkanFG\\Streamline", dir);
    g_sl = LoadLibraryExW(interposer,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    Log("Streamline runtime: %S (%s)", s_slDir, g_sl?"loaded":"LOAD FAILED");
  }
  if(!g_sl){auto native=GetLastError();s_slDir[0]=0;Log("Private Streamline runtime not available; root DLL not substituted");RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Bundled Streamline interposer is missing or could not load",(int)native);}
  return g_sl;
}

// Resolves slInit/slSetVulkanInfo/slGetFeatureFunction from the interposer module
// and calls slInit once. Returns true if slInit succeeded.
static std::mutex g_slInitMutex;
static bool EnsureSlInit(sl::RenderAPI api=sl::RenderAPI::eVulkan){
  std::lock_guard<std::mutex> lock(g_slInitMutex);
  static bool s_tried = false;
  static bool s_armed = false;
  static sl::RenderAPI initializedAPI=api;
  if(s_tried) return s_armed && initializedAPI==api;
  s_tried = true;

  if(!LoadInterposer()){ Log("EnsureSlInit: sl.interposer.dll not found - put the Streamline folder next to fgvk.dll"); return false; }
  if(!PinLocalPlugins()){Log("Local plugin pin failed; leaving Vulkan creation native");RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Local Streamline plugin routing could not be installed");return false;}

  p_slInit = (PFun_slInit*)GetProcAddress(g_sl, "slInit");
  p_slSetVulkanInfo = (PFun_slSetVulkanInfo*)GetProcAddress(g_sl, "slSetVulkanInfo");
  p_slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(g_sl, "slGetFeatureFunction");
  Log("resolved slInit=%p slSetVulkanInfo=%p slGetFeatureFunction=%p",
      (void*)p_slInit, (void*)p_slSetVulkanInfo, (void*)p_slGetFeatureFunction);
  if(!p_slInit || !p_slSetVulkanInfo || !p_slGetFeatureFunction){
    Log("EnsureSlInit: failed to resolve core SL functions");
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Bundled interposer is missing required core exports");
    return false;
  }

  sl::Preferences p{};
  static const sl::Feature feats[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
  p.featuresToLoad = feats;
  p.numFeaturesToLoad = 3;
  // eUseManualHooking IS REQUIRED for the GIPA-interposition model: WE hook vkGetInstanceProcAddr
  // and drive the interposer. Without this flag, slInit tries to install its OWN Vulkan
  // interception, which collides with our in-place GIPA Detour and HANGS slInit (observed: log
  // stops at 'resolved slInit=', no sl.log, game hangs). Manual hooking = slInit skips self-
  // hooking because we're driving. (The earlier BG3SE crash was manual-hooking + a DIFFERENT
  // hybrid that also called slSetVulkanInfo; here we own the whole GIPA chain instead.)
  // Explicit flags: default includes eAllowOTA|eLoadDownloadedPlugins - OTA off (matched 2.12.0).
  p.flags = sl::PreferenceFlags::eDisableCLStateTracking
          | sl::PreferenceFlags::eUseManualHooking
          | sl::PreferenceFlags::eUseFrameBasedResourceTagging;   // slSetTagForFrame needs this
  p.renderAPI = api;
  // RDR2's own observed/authorized NGX application ID is required.
  // A BG3 identity must not silently stand in for RDR2.
  p.applicationId = Cfg().applicationId;
  // Streamline's own verbose log - written next to the game exe; names the exact
  // internal step DLSS-G is on when something wedges.
  p.logLevel = Cfg().debugLogging?sl::LogLevel::eVerbose:sl::LogLevel::eOff;
  p.logMessageCallback=[](sl::LogType type,const char* message){if(type==sl::LogType::eError)Log("[Streamline error] %s",message?message:"<empty>");else LogDebug("[Streamline type=%u] %s",(unsigned)type,message?message:"<empty>");};
  // sl.log next to RDR2.exe (wherever the game is installed)
  static wchar_t s_logDir[MAX_PATH]{};
  { GetModuleFileNameW(nullptr, s_logDir, MAX_PATH); wchar_t* s=wcsrchr(s_logDir,L'\\'); if(s) *s=0; }
  p.pathToLogsAndData = nullptr; // Unified private diagnostic log via callback.
  // Plugin search: Streamline stops at the FIRST directory that holds plugins, so our folder
  // wins and stray sl.*.dll copies next to RDR2.exe are never loaded. NGX, however, receives
  // EVERY path in this list as a feature-DLL search path, and the game's own DLSS Super
  // Resolution DLL (nvngx_dlss.dll) lives next to RDR2.exe - without that second entry NGX
  // reports "nvngx_dlss.dll doesn't exist in any of the search paths", the game's DLSS never
  // starts, and there is nothing for frame generation to attach to (12:30 run).
  static const wchar_t* s_pluginPaths[2];
  if(s_slDir[0]){ s_pluginPaths[0] = s_slDir; s_pluginPaths[1] = s_logDir; p.pathsToPlugins = s_pluginPaths; p.numPathsToPlugins = 2;
    Log("Streamline plugin path: %S ; NGX also searches %S", s_slDir, s_logDir); }

  sl::Result r = p_slInit(p, sl::kSDKVersion);
  Log("slInit -> %d", (int)r);
  s_armed = (r == sl::Result::eOk);
  if(!s_armed)RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"slInit failed",(int)r);
  if(s_armed){
    const wchar_t* names[]={L"sl.common.dll",L"sl.pcl.dll",L"sl.reflex.dll",L"sl.dlss_g.dll"};
    for(auto name:names){
      if(!LocalPluginLoaded(name)){
        Log("Refusing mixed or missing Streamline plugin: %S; native Vulkan fallback",name);
        RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"A required Streamline plugin is missing or loaded from an unexpected directory");
        auto shutdown=(PFun_slShutdown*)GetProcAddress(g_sl,"slShutdown");if(shutdown)shutdown();
        s_armed=false;break;
      }
    }
    if(s_armed && api==sl::RenderAPI::eVulkan && !BindBundledLatency()){
      auto shutdown=(PFun_slShutdown*)GetProcAddress(g_sl,"slShutdown");if(shutdown)shutdown();s_armed=false;
      Log("Latency dependency unavailable; native Vulkan fallback");
      RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Bundled latency dependency could not satisfy Streamline imports");
    }
    if(s_armed){Log("Matched local Streamline plugins verified before Vulkan device creation");RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderInit);}
  }
  return s_armed;
}

// Resolves the DLSS-G / Reflex feature functions via slGetFeatureFunction.
// Must be called AFTER slSetVulkanInfo (the header requires the device to be
// set first: "Macros which use slGetFeatureFunction can only be used AFTER
// device is set by calling either slSetD3DDevice or slSetVulkanInfo").
// First feature lookup starts Streamline plugin initialization, which is not
// safe to enter concurrently from two device-creation threads.
static std::mutex g_featureMutex;
static bool EnsureFeatureFunctions(){
  std::lock_guard<std::mutex> lock(g_featureMutex);
  static bool s_ok = false;
  static ULONGLONG nextAttempt=0;
  if(s_ok)return true;
  const auto now=GetTickCount64();
  if(now<nextAttempt)return false;
  nextAttempt=now+2000;

  if(!p_slGetFeatureFunction){ Log("EnsureFeatureFunctions: slGetFeatureFunction not resolved");RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"Feature lookup export is unavailable"); return false; }

  void* fn = nullptr;
  sl::Result r;

  fn = nullptr;
  // Automatic debugger attachment made the local game exit cleanly during
  // startup. Keep the non-invasive collector; debugger mode is an explicit
  // diagnostic fixture option (RDR2FG_EXTERNAL_DEBUGGER), not a runtime default.
  Log("BOOT STEP begin: slGetFeatureFunction(Reflex, slReflexSetOptions)");
  r = p_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn);
  Log("slGetFeatureFunction(slReflexSetOptions) -> %d fn=%p", (int)r, fn);
  p_slReflexSetOptions = (PFun_slReflexSetOptions*)fn;

  fn = nullptr;
  r = p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn);
  Log("slGetFeatureFunction(slDLSSGSetOptions) -> %d fn=%p", (int)r, fn);
  p_slDLSSGSetOptions = (PFun_slDLSSGSetOptions*)fn;

  fn = nullptr;
  r = p_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn);
  Log("slGetFeatureFunction(slDLSSGGetState) -> %d fn=%p", (int)r, fn);

  // Frame-data + marker functions (core exports + feature fns) for the input pipeline.
  g_slFns.getNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(g_sl,"slGetNewFrameToken");
  g_slFns.setTagForFrame   = (PFun_slSetTagForFrame*)  GetProcAddress(g_sl,"slSetTagForFrame");
  g_slFns.setConstants     = (PFun_slSetConstants*)    GetProcAddress(g_sl,"slSetConstants");
  { void* f=nullptr; p_slGetFeatureFunction(sl::kFeaturePCL,"slPCLSetMarker",f);
    g_slFns.pclSetMarker=(PFun_slPCLSetMarker*)f; }
  { void* f=nullptr; p_slGetFeatureFunction(sl::kFeatureReflex,"slReflexSleep",f);
    g_slFns.reflexSleep=(PFun_slReflexSleep*)f; }
  Log("frame fns: token=%p tag=%p consts=%p pcl=%p sleep=%p",
      (void*)g_slFns.getNewFrameToken,(void*)g_slFns.setTagForFrame,(void*)g_slFns.setConstants,
      (void*)g_slFns.pclSetMarker,(void*)g_slFns.reflexSleep);
  p_slDLSSGGetState = (PFun_slDLSSGGetState*)fn;

  s_ok = p_slReflexSetOptions && p_slDLSSGSetOptions && p_slDLSSGGetState
      && g_slFns.getNewFrameToken && g_slFns.setTagForFrame
      && g_slFns.setConstants && g_slFns.pclSetMarker;
  if(!s_ok){Log("EnsureFeatureFunctions: one or more feature functions failed to resolve");RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"One or more DLSS-G or Reflex feature functions could not be resolved",(int)r);}
  else RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderInit);
  return s_ok;
}

bool EnsureStreamlineInit(){ return EnsureSlInit(); }
bool EnsureStreamlineD3D12Init(){ return EnsureSlInit(sl::RenderAPI::eD3D12); }
void* SlProxyFn(const char* name){
  HMODULE m = LoadInterposer();
  return m ? (void*)GetProcAddress(m, name) : nullptr;
}

static std::atomic<bool> g_deviceFeaturesReady{false};
void OnDeviceCreated(){
  if(g_deviceFeaturesReady.load())return;
  // Full interposition: the interposer created + owns the device and its queues (its own
  // surgery). NO slSetVulkanInfo, NO device surgery - those double-register an interposer
  // device and caused every crash/queue-rejection before. We only enable the features.
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit not ready"); return; }
  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature function resolution failed"); return; }

  // Establish the viewport's mode before swapchain creation and the first
  // present. The host gate being false does not initialize plugin options.
  SetDLSSGeneration(false);
  LogModules();

  // DLSS-G requires Reflex ON. Set Reflex now; DLSS-G is enabled later by the eval-driven gate.
  sl::ReflexOptions ro{};
  ro.mode = Cfg().reflexMode!=2 ? sl::ReflexMode::eLowLatency
                                : sl::ReflexMode::eLowLatencyWithBoost;   // PureDark config: mReflexMode=2 (Boost), no cap
  sl::Result rReflex = p_slReflexSetOptions(ro);
  Log("slReflexSetOptions(mode=%d) -> %d (DLSS-G itself waits for the eval-driven gate)", Cfg().reflexMode, (int)rReflex);
  g_deviceFeaturesReady.store(rReflex==sl::Result::eOk);
}

void ApplyNativeFrameBudget(){
 static uint32_t previous=UINT32_MAX;
 const auto state=GetFGSnapshot();
 const uint32_t desired=FrameBudgetUs(Rt().fpsCap.load(),Rt().outputFPSCap.load(),state.requested && state.accepted?Rt().frames+1:1);
 if(!p_slReflexSetOptions || desired==previous)return;
 sl::ReflexOptions options{};options.mode=Cfg().reflexMode==2?sl::ReflexMode::eLowLatencyWithBoost:sl::ReflexMode::eLowLatency;options.frameLimitUs=desired;
 if(p_slReflexSetOptions(options)==sl::Result::eOk)previous=desired;
}

// Enable/suspend DLSS-G generation. Driven by the eval-driven gate in vkhooks (on the present
// thread, guide 6.0). Every call - on AND off - carries eRetainResourcesWhenOff: without it,
// eOff frees every FG resource and the next eOn re-creates the feature, a ~140ms frame that
// Streamline itself flags as "Frame rate over 100ms". Both fatal WaitSemaphores timeouts in
// the 09-03 logs sit inside that free/re-create cycle. Guide 6.4: "strongly recommended".
static uint32_t g_framesMax = 0;
static std::mutex g_stateMutex;
static FGSnapshot g_fgState;
static uint32_t g_windowPolls{},g_windowValid{};
static uint64_t g_windowShown{};
static uint32_t g_zeroTelemetryPolls{};
FGSnapshot GetFGSnapshot(){std::lock_guard<std::mutex> lock(g_stateMutex);return g_fgState;}

static void OnDLSSGApiError(const sl::APIError& error){
  RuntimeErrors::ReportError(RuntimeErrors::Code::PresentFailed,"DLSS-G reported an underlying Vulkan present/acquire failure",(int)error.vkRes);
  Log("DLSS-G API error callback: vkRes=%d",(int)error.vkRes);
}
bool SetDLSSGeneration(bool on){
  if(on && !g_deviceFeaturesReady.load()){
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,"DLSS-G enable deferred until feature functions and Reflex initialization succeed");
    return false;
  }
  {std::lock_guard<std::mutex> lock(g_stateMutex);g_fgState.requested=on;++g_fgState.optionCalls;}
  if(!p_slDLSSGSetOptions){
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_fgState.optionsResult=-1;g_fgState.state=FGState::Error;
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"slDLSSGSetOptions is unavailable; generation cannot be requested");
    return false;
  }
  sl::DLSSGOptions o{};
  o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  uint32_t n = Rt().frames; if(n<1) n=1; if(g_framesMax && n > g_framesMax) n = g_framesMax;
  o.numFramesToGenerate = n;
  o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff | sl::DLSSGFlags::eEnableFullscreenMenuDetection;
  o.onErrorCallback = &OnDLSSGApiError;
  // Native HUD capture supplies a HUD-less scene, not a UI alpha layer.
  // Do not enable recomposition using the old transparent placeholder.
  o.enableUserInterfaceRecomposition = Cfg().tagUI && !Cfg().nativeHudProtection ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  sl::ViewportHandle vp{0};
  sl::Result r = p_slDLSSGSetOptions(vp, o);
  const bool accepted=r==sl::Result::eOk || r==sl::Result::eWarnOutOfVRAM;
  static_assert(static_cast<int>(sl::Result::eWarnOutOfVRAM)==39,"Update result classification for new SDK");
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_fgState.optionsResult=(int)r;
    if(accepted){g_fgState.accepted=on;g_fgState.measuredRatio=0;g_windowPolls=g_windowValid=0;g_windowShown=0;g_zeroTelemetryPolls=0;}
    g_fgState.state=ClassifyFG(on,g_fgState.accepted,!accepted,false,0);
  }
  RuntimeErrors::SetWarning(RuntimeErrors::Warning::VRAM,r==sl::Result::eWarnOutOfVRAM);
  RuntimeErrors::SetWarning(RuntimeErrors::Warning::Telemetry,false);
  if(accepted){
    UpdateMenuStats(g_framesMax,0,0);
    SetFPSGenerationActive(on);
    RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderOptions);
  }else RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"slDLSSGSetOptions rejected the requested mode; see native result",(int)r);
  Log("SetDLSSGeneration(%d) frames=%u retainResources=1 -> %d", (int)on, n, (int)r);
  return accepted;
}

// Present thread only (slDLSSGGetState is not thread safe and SL wants it synced with present).
// Null options = status + frame stats only, no VRAM estimate (guide 13.0 / 17.0).
static PFun_slIsFeatureSupported* p_slIsFeatureSupported{};
void RearmDLSSGeneration(){
  if(!p_slDLSSGSetOptions)return;
  bool req=false;{std::lock_guard<std::mutex> lock(g_stateMutex);req=g_fgState.requested;}
  if(req){Log("DLSS-G re-arm: options re-applied (preset/provider refresh)");SetDLSSGeneration(true);}
}
bool RestartNativeFGResources(unsigned preset){
  if(Cfg().provider!=FGProvider::DLSSG || preset>3 || GetFGSnapshot().accepted){
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"Native FG restart requires the feature to be disabled first");return false;
  }
  const auto freeResources=g_sl?(PFun_slFreeResources*)GetProcAddress(g_sl,"slFreeResources"):nullptr;
  if(!freeResources){RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"slFreeResources is unavailable; FG restart cannot continue");return false;}
  const auto result=freeResources(sl::kFeatureDLSS_G,sl::ViewportHandle{0});
  if(!FGResultAccepted(int(result))){RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"Native DLSS-G resource reset failed",int(result));return false;}
  // The feature is off and its viewport resources are gone. Keep the Vulkan
  // device, swapchain and loaded plugin intact; the next valid input rebuilds FG.
  const auto core=GetModuleHandleW(L"RTX40MFGCore.dll");
  using SetPreset=BOOL(WINAPI*)(uint32_t);
  const auto setPreset=core?(SetPreset)GetProcAddress(core,"MfgUnlockSetPreset"):nullptr;
  if(setPreset){
    if(!setPreset(preset)){RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"The loaded provider cannot apply this preset; check preset feedback and retry");return false;}
    Rt().appliedPreset=int(preset);
  }else if(int(preset)!=Rt().appliedPreset.load()){
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"Live preset control is unavailable on this provider; restore the loaded preset or use the NVIDIA profile and restart the game");return false;
  }
  RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderOptions);
  RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderState);
  RuntimeErrors::ClearError(RuntimeErrors::Code::ConstantsFailed);
  RuntimeErrors::ClearError(RuntimeErrors::Code::FrameIndex);
  Log("Native FG restart: viewport resources released; requested preset=%u; awaiting fresh SR inputs",preset);
  return true;
}
void PollDLSSGState(){
  if(!g_deviceFeaturesReady.load()){
    static ULONGLONG retryAt=0;const auto now=GetTickCount64();
    if(now>=retryAt){retryAt=now+2000;OnDeviceCreated();}
    if(!g_deviceFeaturesReady.load())return;
  }
  if(!p_slDLSSGGetState){static bool reported=false;if(!reported){reported=true;RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderState,"slDLSSGGetState is unavailable; displayed FPS is not verified");}return;}
  if(!p_slIsFeatureSupported)p_slIsFeatureSupported=(PFun_slIsFeatureSupported*)GetProcAddress(g_sl,"slIsFeatureSupported");
  { static bool supportQueried=false;
    if(!supportQueried && p_slIsFeatureSupported){
      supportQueried=true;
      sl::AdapterInfo adapter{};adapter.vkPhysicalDevice=(void*)gPhysicalDevice;
      auto sr=p_slIsFeatureSupported(sl::kFeatureDLSS_G,adapter);
      std::lock_guard<std::mutex> lock(g_stateMutex);
      g_fgState.dlssgSupportKnown=true;g_fgState.dlssgSupported=(sr==sl::Result::eOk);
      Log("DLSS-G support query: result=%d supported=%d",(int)sr,(int)g_fgState.dlssgSupported);
    } }
  sl::DLSSGState st{};
  bool doRearm=false;float rearmRatio=0;
  sl::ViewportHandle vp{0};
  auto result=p_slDLSSGGetState(vp, st, nullptr);
  const bool valid=result==sl::Result::eOk || result==sl::Result::eWarnOutOfVRAM;
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if(!g_fgState.accepted || (valid&&st.numFramesActuallyPresented>0))g_zeroTelemetryPolls=0;
    else ++g_zeroTelemetryPolls;
    RuntimeErrors::SetWarning(RuntimeErrors::Warning::Telemetry,g_fgState.accepted&&g_zeroTelemetryPolls>=60);
  }
  static int lastQueryResult=-1;
  if((int)result!=lastQueryResult){
    lastQueryResult=(int)result;
    Log("slDLSSGGetState result=%d acceptedTelemetry=%u presented=%u status=0x%x framesMax=%u%s",(int)result,(unsigned)valid,st.numFramesActuallyPresented,(unsigned)st.status,st.numFramesToGenerateMax,result==sl::Result::eWarnOutOfVRAM?" warning=OutOfVRAM":"");
  }
  if(result==sl::Result::eWarnOutOfVRAM)RuntimeErrors::SetWarning(RuntimeErrors::Warning::VRAM,true);
  if(valid){
    RecordDisplayedFrames(st.numFramesActuallyPresented);
    PerformanceProbeDisplayed(st.numFramesActuallyPresented);
    if(st.numFramesToGenerateMax){
      const uint32_t maximum=std::min(5u,st.numFramesToGenerateMax);
      if(maximum!=g_framesMax)Log("DLSS-G multiplier capacity: GPU=%s generatedMax=%u exposed=%ux unlock=%s",GPUName(),st.numFramesToGenerateMax,maximum+1,GPUUnlockStatus());
      g_framesMax=maximum;
    }
    if((uint32_t)st.status){
      char detail[256];std::snprintf(detail,sizeof(detail),"DLSS-G status bits=0x%x: resolutionLow=%u reflexMissing=%u unsupportedHDR=%u invalidConstants=%u backbufferIndex=%u",(unsigned)st.status,!!((unsigned)st.status&1),!!((unsigned)st.status&2),!!((unsigned)st.status&4),!!((unsigned)st.status&8),!!((unsigned)st.status&16));
      RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderState,detail,(int)st.status);
    }else RuntimeErrors::ClearError(RuntimeErrors::Code::ProviderState);
  }else RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderState,"slDLSSGGetState failed; displayed FPS is not verified",(int)result);
  static uint32_t last = 0xFFFFFFFF;
  if(valid && (uint32_t)st.status != last){
    last = (uint32_t)st.status;
    Log("DLSSG status=%u framesMax=%u vsyncSupport=%u", (unsigned)st.status, st.numFramesToGenerateMax,
        (unsigned)st.bIsVsyncSupportAvailable);
  }
  // numFramesActuallyPresented = frames displayed since the previous GetState call, so summed
  // over N presents it is the real multiplier (x1 = nothing generated, ~x4 = MFG working).
  {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    ++g_fgState.polls;++g_windowPolls;g_fgState.queryResult=(int)result;
    if(valid){
      if(result==sl::Result::eWarnOutOfVRAM)++g_fgState.queryWarnings;else ++g_fgState.querySuccess;
      ++g_windowValid;g_windowShown+=st.numFramesActuallyPresented;
      g_fgState.lastPresented=st.numFramesActuallyPresented;g_fgState.status=(uint32_t)st.status;
    }else{++g_fgState.queryErrors;g_fgState.measuredRatio=0;}
    // Publish after a short real observation window; 300 presents is for logging,
    // not a prerequisite for the menu to acknowledge measured generation.
    if(g_windowPolls>=30){
      const bool measured=g_windowValid==g_windowPolls && g_windowShown>0;
      g_fgState.measuredRatio=measured?float(g_windowShown)/g_windowPolls:0;
      UpdateMenuStats(g_framesMax,g_fgState.status,g_fgState.measuredRatio);
    }
    if(g_windowPolls>=300){
      // A missed query returns a cumulative count next time. Never divide that by
      // successful queries only: the denominator is all game-present polls.
      const bool measured=g_windowValid==g_windowPolls && g_windowShown>0;
      g_fgState.measuredRatio=measured?float(g_windowShown)/g_windowPolls:0;
      UpdateMenuStats(g_framesMax,g_fgState.status,g_fgState.measuredRatio);
      Log("FG stats: polls=%u valid=%u displayed=%llu measuredRatio=%.3f optionsCalls=%llu queryOK=%llu queryWarn=%llu queryError=%llu optionsResult=%d queryResult=%d status=0x%x",g_windowPolls,g_windowValid,(unsigned long long)g_windowShown,g_fgState.measuredRatio,(unsigned long long)g_fgState.optionCalls,(unsigned long long)g_fgState.querySuccess,(unsigned long long)g_fgState.queryWarnings,(unsigned long long)g_fgState.queryErrors,g_fgState.optionsResult,g_fgState.queryResult,g_fgState.status);
      g_windowPolls=g_windowValid=0;g_windowShown=0;
    }
    g_fgState.state=ClassifyFG(g_fgState.requested,g_fgState.accepted,!FGResultAccepted(g_fgState.optionsResult) || !valid || (valid&&(uint32_t)st.status!=0),g_fgState.measuredRatio>0,g_fgState.measuredRatio);
    // Stuck-generation recovery: options accepted, inputs flowing, status OK,
    // yet DLSS-G presents no generated frames for 5s (seen after an in-game
    // DLSS quality change recreates the SR feature). Re-apply the options
    // once so the plugin re-arms interpolation; 15s cooldown, no gate desync
    // because requested/accepted bookkeeping is untouched.
    { static ULONGLONG unmeasuredSince=0,lastRearm=0;const ULONGLONG nowMs=GetTickCount64();
      const bool stuck=g_fgState.requested&&g_fgState.accepted&&g_fgState.measuredRatio<=1.01f&&(uint32_t)st.status==0;
      if(stuck){ if(!unmeasuredSince)unmeasuredSince=nowMs;
        else if(nowMs-unmeasuredSince>=5000&&nowMs-lastRearm>=15000){ lastRearm=nowMs;unmeasuredSince=0;doRearm=true;rearmRatio=g_fgState.measuredRatio; } }
      else unmeasuredSince=0; }
  }
  // SetDLSSGeneration locks g_stateMutex itself; calling it while this
  // function's lock_guard held self-deadlocked the present thread (freeze
  // after preset changes). Always re-arm after the lock is released.
  if(doRearm){ Log("DLSS-G re-arm: accepted but not generating for 5s (ratio=%.3f); options re-applied",rearmRatio); SetDLSSGeneration(true); }
}
}

