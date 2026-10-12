#include "log.h"
#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "slboot.h"
#include "menu.h"
#include "vkhooks.h"
#include "config.h"
#include "performance_probe.h"

#include <sl.h>
#include <sl_helpers_vk.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_consts.h>
#include <vector>
#include <string>
#include <algorithm>
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
  if(GetModuleHandleA("sl.interposer.dll")){Log("Another interposer is loaded; refusing mixed runtime");return nullptr;}
  HMODULE self{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)&LoadInterposer,&self);
  wchar_t dir[MAX_PATH]{}; GetModuleFileNameW(self, dir, MAX_PATH); wchar_t* s=wcsrchr(dir,L'\\'); if(s) *(s+1)=0;
  wchar_t interposer[MAX_PATH]; swprintf(interposer, MAX_PATH, L"%sRDR2VulkanFG\\Streamline\\sl.interposer.dll", dir);
  if(GetFileAttributesW(interposer)!=INVALID_FILE_ATTRIBUTES){
    swprintf(s_slDir, MAX_PATH, L"%sRDR2VulkanFG\\Streamline", dir);
    g_sl = LoadLibraryExW(interposer,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    Log("Streamline runtime: %S (%s)", s_slDir, g_sl?"loaded":"LOAD FAILED");
  }
  if(!g_sl){s_slDir[0]=0;Log("Private Streamline runtime not available; root DLL not substituted");}
  return g_sl;
}

// Resolves slInit/slSetVulkanInfo/slGetFeatureFunction from the interposer module
// and calls slInit once. Returns true if slInit succeeded.
static bool EnsureSlInit(){
  static bool s_tried = false;
  static bool s_armed = false;
  if(s_tried) return s_armed;
  s_tried = true;

  if(!LoadInterposer()){ Log("EnsureSlInit: sl.interposer.dll not found - put the Streamline folder next to fgvk.dll"); return false; }
  if(!PinLocalPlugins()){Log("Local plugin pin failed; leaving Vulkan creation native");return false;}

  p_slInit = (PFun_slInit*)GetProcAddress(g_sl, "slInit");
  p_slSetVulkanInfo = (PFun_slSetVulkanInfo*)GetProcAddress(g_sl, "slSetVulkanInfo");
  p_slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(g_sl, "slGetFeatureFunction");
  Log("resolved slInit=%p slSetVulkanInfo=%p slGetFeatureFunction=%p",
      (void*)p_slInit, (void*)p_slSetVulkanInfo, (void*)p_slGetFeatureFunction);
  if(!p_slInit || !p_slSetVulkanInfo || !p_slGetFeatureFunction){
    Log("EnsureSlInit: failed to resolve core SL functions");
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
  p.renderAPI = sl::RenderAPI::eVulkan;
  // RDR2's own observed/authorized NGX application ID is required.
  // A BG3 identity must not silently stand in for RDR2.
  p.applicationId = Cfg().applicationId;
  // Streamline's own verbose log - written next to the game exe; names the exact
  // internal step DLSS-G is on when something wedges.
  p.logLevel = sl::LogLevel::eDefault;
  // sl.log next to RDR2.exe (wherever the game is installed)
  static wchar_t s_logDir[MAX_PATH]{};
  { GetModuleFileNameW(nullptr, s_logDir, MAX_PATH); wchar_t* s=wcsrchr(s_logDir,L'\\'); if(s) *s=0; }
  p.pathToLogsAndData = s_logDir;
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
  if(s_armed){
    const wchar_t* names[]={L"sl.common.dll",L"sl.pcl.dll",L"sl.reflex.dll",L"sl.dlss_g.dll"};
    for(auto name:names){
      if(!LocalPluginLoaded(name)){
        Log("Refusing mixed or missing Streamline plugin: %S; native Vulkan fallback",name);
        auto shutdown=(PFun_slShutdown*)GetProcAddress(g_sl,"slShutdown");if(shutdown)shutdown();
        s_armed=false;break;
      }
    }
    if(s_armed)Log("Matched local Streamline plugins verified before Vulkan device creation");
  }
  return s_armed;
}

// Resolves the DLSS-G / Reflex feature functions via slGetFeatureFunction.
// Must be called AFTER slSetVulkanInfo (the header requires the device to be
// set first: "Macros which use slGetFeatureFunction can only be used AFTER
// device is set by calling either slSetD3DDevice or slSetVulkanInfo").
static bool EnsureFeatureFunctions(){
  static bool s_tried = false;
  static bool s_ok = false;
  if(s_tried) return s_ok;
  s_tried = true;

  if(!p_slGetFeatureFunction){ Log("EnsureFeatureFunctions: slGetFeatureFunction not resolved"); return false; }

  void* fn = nullptr;
  sl::Result r;

  fn = nullptr;
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

  s_ok = p_slReflexSetOptions && p_slDLSSGSetOptions && p_slDLSSGGetState;
  if(!s_ok) Log("EnsureFeatureFunctions: one or more feature functions failed to resolve");
  return s_ok;
}

bool EnsureStreamlineInit(){ return EnsureSlInit(); }
void* SlProxyFn(const char* name){
  HMODULE m = LoadInterposer();
  return m ? (void*)GetProcAddress(m, name) : nullptr;
}

void OnDeviceCreated(){
  // Full interposition: the interposer created + owns the device and its queues (its own
  // surgery). NO slSetVulkanInfo, NO device surgery - those double-register an interposer
  // device and caused every crash/queue-rejection before. We only enable the features.
  if(!EnsureSlInit()) { Log("OnDeviceCreated: slInit not ready"); return; }
  if(!EnsureFeatureFunctions()) { Log("OnDeviceCreated: feature function resolution failed"); return; }

  // DLSS-G requires Reflex ON. Set Reflex now; DLSS-G is enabled later by the eval-driven gate.
  sl::ReflexOptions ro{};
  ro.mode = Cfg().reflexMode==0 ? sl::ReflexMode::eOff : Cfg().reflexMode==1 ? sl::ReflexMode::eLowLatency
                                : sl::ReflexMode::eLowLatencyWithBoost;   // PureDark config: mReflexMode=2 (Boost), no cap
  sl::Result rReflex = p_slReflexSetOptions(ro);
  Log("slReflexSetOptions(mode=%d) -> %d (DLSS-G itself waits for the eval-driven gate)", Cfg().reflexMode, (int)rReflex);
}

// Enable/suspend DLSS-G generation. Driven by the eval-driven gate in vkhooks (on the present
// thread, guide 6.0). Every call - on AND off - carries eRetainResourcesWhenOff: without it,
// eOff frees every FG resource and the next eOn re-creates the feature, a ~140ms frame that
// Streamline itself flags as "Frame rate over 100ms". Both fatal WaitSemaphores timeouts in
// the 09-03 logs sit inside that free/re-create cycle. Guide 6.4: "strongly recommended".
static uint32_t g_framesMax = 0;                 // DLSSGState::numFramesToGenerateMax (0 = not read yet)

void SetDLSSGeneration(bool on){
  if(!p_slDLSSGSetOptions) return;
  sl::DLSSGOptions o{};
  o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  uint32_t n = Rt().frames; if(n<1) n=1; if(g_framesMax && n > g_framesMax) n = g_framesMax;
  o.numFramesToGenerate = n;
  o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
  sl::ViewportHandle vp{0};
  sl::Result r = p_slDLSSGSetOptions(vp, o);
  if(r==sl::Result::eOk)SetFPSGenerationActive(on);
  Log("SetDLSSGeneration(%d) frames=%u retainResources=1 -> %d", (int)on, n, (int)r);
}

// Present thread only (slDLSSGGetState is not thread safe and SL wants it synced with present).
// Null options = status + frame stats only, no VRAM estimate (guide 13.0 / 17.0).
void PollDLSSGState(){
  if(!p_slDLSSGGetState) return;
  sl::DLSSGState st{};
  sl::ViewportHandle vp{0};
  auto result=p_slDLSSGGetState(vp, st, nullptr);
  if(result!=sl::Result::eOk){static auto lastResult=sl::Result::eOk;if(lastResult!=result){lastResult=result;Log("DLSS-G statistics unavailable: result=%d; game FPS sampling continues",(int)result);}return;}
  RecordDisplayedFrames(st.numFramesActuallyPresented);
  PerformanceProbeDisplayed(st.numFramesActuallyPresented);
  if(st.numFramesToGenerateMax) g_framesMax = st.numFramesToGenerateMax;
  static uint32_t last = 0xFFFFFFFF;
  if((uint32_t)st.status != last){
    last = (uint32_t)st.status;
    Log("DLSSG status=%u framesMax=%u vsyncSupport=%u", (unsigned)st.status, st.numFramesToGenerateMax,
        (unsigned)st.bIsVsyncSupportAvailable);
  }
  // numFramesActuallyPresented = frames displayed since the previous GetState call, so summed
  // over N presents it is the real multiplier (x1 = nothing generated, ~x4 = MFG working).
  static uint32_t polls = 0, shown = 0;
  polls++; shown += st.numFramesActuallyPresented;
  if(polls >= 300){
    UpdateMenuStats(g_framesMax,(uint32_t)st.status,shown/(float)polls);
    Log("FG stats: %u presents -> %u frames displayed (x%.2f) status=%u", polls, shown, shown/(double)polls, (unsigned)st.status);
    polls = 0; shown = 0;
  }
}
}

