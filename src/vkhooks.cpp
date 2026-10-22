#include "vkhooks.h"
#include "exportroute.h"
#include "slmodule.h"
#include "log.h"
#include "slboot.h"
#include "inputs.h"
#include "input_capture.h"
#include "render_reprojection.h"
#include "config.h"
#include "runtime_errors.h"
#include "presentpacer.h"
#include "present_compat.h"
#include "present_route.h"
#include <mutex>
#include <algorithm>
#include <windows.h>
#include <detours.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>
#include <vector>
#include "resource_state.h"
#include "ampere_boot.h"
#include "menu.h"
#include "swapchain_lock.h"
#include "xess_provider.h"
#include "xess_queue_sync.h"
#include "hud_capture.h"
#include "performance_probe.h"
#include "vulkan_owner.h"
#include "gpu_guard.h"
#include "native_restart.h"
#include "native_hud.h"
#include <vulkan/vulkan_win32.h>

namespace fgvk {
static bool CallerIsStreamline(void*);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeBackendGDPA(VkDevice,const char*);
static PFN_vkVoidFunction XessNativeExport(const char*);
static PFN_vkCreateWin32SurfaceKHR o_CreateWin32SurfaceKHR_export{};
static PFN_vkDestroySwapchainKHR o_DestroySwapchainKHR_export{};
static PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR_export{};
static PFN_vkGetSwapchainImagesKHR o_GetSwapchainImagesKHR_export{};
VkInstance gInstance{};
VkPhysicalDevice gPhysicalDevice{};
VkDevice gDevice{};
VkQueue gGraphicsQueue{};
uint32_t gGraphicsFamily{};
static VulkanOwner g_owner;
static std::atomic<VkInstance> g_ownedInstance{nullptr};
static std::atomic<VkDevice> g_ownedDevice{nullptr};
static std::atomic<bool> frameGenerationAvailable{true};

// watchdog: counters + a position marker naming the in-flight call (inputs.cpp writes g_wdEvals/g_wdPos)
std::atomic<uint32_t> g_wdPresents{0};
std::atomic<uint32_t> g_wdEvals{0};
// pos: 0 idle (inside the game); 1 present-enter; 2 inside proxy present; 3 post-present
// markers/Reflex sleep; 4 game vkAcquireNextImageKHR; 5 vkWaitForFences; 6 vkQueueSubmit(2);
// 7 vkDeviceWaitIdle; 8 vkQueueWaitIdle; 9 present pacing wait; 10/11/12 eval stages
std::atomic<int> g_wdPos{0};
static std::atomic<long long> g_wdPosSinceUs{0};
static std::atomic<bool> g_genOn{false};   // DLSS-G currently requested on (gate state)
static inline long long NowUs(){ static const double factor=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1e6/double(f.QuadPart);}();LARGE_INTEGER c{};QueryPerformanceCounter(&c);return (long long)(double(c.QuadPart)*factor); }
// Position tracking is for the present thread only: NGX/SL worker threads can reach our
// wrappers through the exported vkGetDeviceProcAddr and must not clobber it.
static std::atomic<uint32_t> g_presentTid{0};
struct PosScope {
  int prev; bool on;
  PosScope(int code){ uint32_t t=g_presentTid.load(); on = (t==0 || t==(uint32_t)GetCurrentThreadId());
    if(on){ prev=g_wdPos.exchange(code); g_wdPosSinceUs.store(NowUs()); } }
  ~PosScope(){ if(on){ g_wdPos.store(prev); g_wdPosSinceUs.store(NowUs()); } }
};

static void StartWatchdog(){
  static std::atomic<bool> started{false};
  bool exp=false; if(!started.compare_exchange_strong(exp,true)) return;
  StartDiagnosticCollector();
  std::thread([]{
    uint32_t lastP=0; int stalledTicks=0; int dumps=0;
    for(;;){ Sleep(2000);
      uint32_t p=g_wdPresents.load(), e=g_wdEvals.load(); int pos=g_wdPos.load();
      if(p!=lastP && pos==0){ lastP=p; stalledTicks=0; continue; }
      bool stalled = (p==lastP);
      double inPos = (NowUs()-g_wdPosSinceUs.load())/1e6;
      Log("wd: presents=%u evals=%u pos=%d (%.1fs there) gen=%d%s", p,e,pos,inPos,(int)g_genOn.load(),stalled?" STALLED":"");
      stalledTicks = stalled ? stalledTicks+1 : 0;
      if(p>0 && stalledTicks==3){char detail[128];snprintf(detail,sizeof(detail),"No present for ~6s; watchdog pos=%d evals=%u gen=%d",pos,e,(int)g_genOn.load());RuntimeErrors::ReportError(RuntimeErrors::Code::GenerationStalled,detail,pos);}
      if(!stalled)RuntimeErrors::ClearError(RuntimeErrors::Code::GenerationStalled);
      // first dump after ~6s of no presents, a second one ~10s later to see what moved
      if(p>0 && ((stalledTicks==3 && dumps==0) || (stalledTicks==8 && dumps==1))){ dumps++; if(DebugLoggingEnabled()){Log("Debug: requesting external thread stacks for stalled presentation");RequestDiagnosticStacks();} }
      lastP=p; }
  }).detach();
}

// DLSS-G gate: driven by whether the game's DLSS-SR evaluated this frame (= the 3D world is
// rendering), NEVER by frame time. The previous frame-time gate manufactured a full FG
// release + re-create at every in-world streaming burst; both fatal WaitSemaphores timeouts
// (19:51 and 21:23 runs) sit inside that cycle, and PureDark rides through the same hitches
// with FG left on. Loading screens and videos may stop DLSS-SR; pause menus can keep it running.
// Pause menus that continue rendering DLSS are not inferred from keyboard or camera movement.
// "No eval for a while"
// is the guide's own "turn FG off when not rendering game frames" signal (17.0) - and with
// eRetainResourcesWhenOff those transitions free nothing. Loading screens also call
// vkDeviceWaitIdle from a loader thread (sl.log tid 540); with FG suspended there that flush
// meets an idle pacer.
static uint32_t g_evalStreak = 0, g_idleStreak = 0;
static ULONGLONG g_lastEvalTime{};
#define kOnAfterEvalFrames  (Cfg().onAfterEvalFrames)    // ~2s of world rendering before enabling
#define kOffAfterIdleFrames (Cfg().offAfterIdleFrames)   // ~0.5-1s of presents without DLSS-SR before suspending
static bool RequestGeneration(bool on){
  if(!SetDLSSGeneration(on))return false;
  g_genOn.store(on);return true;
}
static NativeRestartState nativeRestart;
static ULONGLONG nativeRestartSince{};
void RequestNativeRestart(unsigned preset){
  if(Cfg().provider!=FGProvider::DLSSG || !frameGenerationAvailable || !gDevice)return;
  if(nativeRestart.Request(preset)){nativeRestartSince=GetTickCount64();Log("Native FG restart queued; requested preset=%u",preset);}
}
bool NativeRestartPending(){return nativeRestart.Pending();}
bool NativeRestartFailed(){return nativeRestart.Failed() && !g_genOn.load();}
static void ProcessNativeRestart(){
  if(!nativeRestart.Pending())return;
  if(nativeRestartSince && GetTickCount64()-nativeRestartSince>5000){
    nativeRestart.Complete(false);
    RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderOptions,"Native FG restart timed out while waiting for recording/presentation to retire; retry after returning to gameplay");return;
  }
  if(nativeRestart.Next()==NativeRestartState::Action::Disable){
    nativeRestart.Disabled(RequestGeneration(false));g_evalStreak=g_idleStreak=0;return;
  }
  if(nativeRestart.Next()==NativeRestartState::Action::Rebuild){
    const auto reset=ResetNativeFGInputs(nativeRestart.Preset());
    if(reset==NativeInputReset::Busy)return;
    nativeRestart.Complete(reset==NativeInputReset::Complete);
    g_evalStreak=g_idleStreak=0;g_lastEvalTime=0;
  }
}
static void EvalGate(bool evalThisFrame){
  static uint32_t appliedFrames=0;
  static bool initializedOnce=false;
  static ULONGLONG nextEnableAttempt=0,nextDisableAttempt=0;
  bool changed=PollHotkeys();changed=PerformanceProbeBegin(evalThisFrame)||changed;
  if(NativeRestartPending() || NativeRestartFailed()){g_evalStreak=0;return;}
  const auto now=GetTickCount64();
  if(Rt().fgUserOff || !HasRecentDLSSInput()){
    if(g_genOn.load() && (changed || now>=nextDisableAttempt)){
      if(RequestGeneration(false))Log("gate: DLSS-G suspended; reason=%s SRageMs=%llu graceMs=%u",Rt().fgUserOff.load()?"user disabled":"no successful valid DLSS-SR input",(unsigned long long)DLSSInputAgeMs(),Cfg().noDLSSGraceMs);
      nextDisableAttempt=now+1000;
    }
    g_evalStreak=0;g_idleStreak=0;g_lastEvalTime=0;nextEnableAttempt=now;return;
  }
  if(g_genOn.load() && Rt().frames!=appliedFrames && now>=nextEnableAttempt){
    if(RequestGeneration(true))appliedFrames=Rt().frames;
    nextEnableAttempt=now+1000;
  }
  if(evalThisFrame){
    g_lastEvalTime=now;g_idleStreak=0;
    if(g_evalStreak<100000)++g_evalStreak;
    const uint32_t requiredFrames=initializedOnce?1:kOnAfterEvalFrames;
    if(!g_genOn.load() && g_evalStreak>=requiredFrames && now>=nextEnableAttempt){
      if(RequestGeneration(true)){appliedFrames=Rt().frames;nextDisableAttempt=0;initializedOnce=true;Log("gate: %u valid DLSS-SR frames -> DLSS-G enabled (initialization=%u)",g_evalStreak,(unsigned)requiredFrames);}
      nextEnableAttempt=now+1000;
    }
  }else{
    if(g_idleStreak<100000)++g_idleStreak;
    const bool expired=now-g_lastEvalTime>=Cfg().noDLSSGraceMs;
    if(expired)g_evalStreak=0;
    if(g_genOn.load() && expired && g_idleStreak>=kOffAfterIdleFrames && now>=nextDisableAttempt){
      if(RequestGeneration(false)){nextEnableAttempt=now;Log("gate: DLSS-G suspended; reason=present inputs unavailable idlePresents=%u lastReadyAgeMs=%llu SRageMs=%llu",g_idleStreak,(unsigned long long)(now-g_lastEvalTime),(unsigned long long)DLSSInputAgeMs());}
      nextDisableAttempt=now+1000;
    }
  }
}
// Guide 17.0: DLSS-G off before any swap-chain manipulation "to avoid potential deadlocks".
static void GateOffForSwapchain(){
  g_evalStreak = 0; g_idleStreak = 0;
  if(g_genOn.load()){ if(RequestGeneration(false))Log("gate: swapchain re-create -> DLSS-G suspended first"); }
}

// The game's own vkGetInstanceProcAddr (real loader), captured at hook install.
static PFN_vkGetInstanceProcAddr o_GIPA{};
static PFN_vkGetDeviceProcAddr o_GDPA_real{};
// The interposer's GIPA/GDPA - SL owns every function these hand back.
static PFN_vkGetInstanceProcAddr ip_GIPA{};
static PFN_vkGetDeviceProcAddr   ip_GDPA{};
// Interposer targets we thin-wrap (resolved lazily via the interposer's GIPA/GDPA).
static PFN_vkCreateInstance     t_CreateInstance{};
static PFN_vkCreateDevice       t_CreateDevice{};
static PFN_vkQueuePresentKHR    o_QueuePresentKHR_export{};   // real loader present (trampoline once hooked)
static PFN_vkQueuePresentKHR    t_QueuePresentKHR{};
static PFN_vkCreateSwapchainKHR t_CreateSwapchainKHR{};

// Re-entry guard: we Detoured vulkan-1's GIPA in place, so when the interposer forwards to
// the real loader (to reach the driver) it re-enters our hook. Without this, our hook routes
// that back into the interposer -> infinite recursion (the slInit hang). Calls made while
// g_reentry>0 originate INSIDE the interposer and must go straight to the real loader.
static thread_local int g_reentry = 0;
struct Reentry { Reentry(){ ++g_reentry; } ~Reentry(){ --g_reentry; } };
// Global guard for slInit: SL's plugins spawn WORKER THREADS during init that call the hooked
// GIPA (where thread_local g_reentry is 0). The game makes no Vulkan calls during slInit, so
// forcing ALL threads to the real loader for that window is safe and breaks the cross-thread
// recursion that hangs slInit.
static std::atomic<int> g_globalReal{0};
struct GlobalReal { GlobalReal(){ ++g_globalReal; } ~GlobalReal(){ --g_globalReal; } };
static inline bool ForceReal(){ return g_reentry || g_globalReal.load(std::memory_order_acquire) || IsGpuBlocked(); }
// Loader vkCreateDevice window: set while the loader's EXPORTED vkCreateDevice executes on this
// thread (our detour on that export is attached late, in w_CreateInstance, so it is the outermost
// one and covers other mods' post-create work). See exportroute.h.
static thread_local int g_loaderCreateDevice = 0;
static thread_local int g_loaderCreateReentry = 0;   // our re-entry depth when the window opened
static PFN_vkCreateDevice o_CreateDevice_export{};
static PFN_vkCreateInstance o_CreateInstance_export{};
static PFN_vkEnumerateDeviceExtensionProperties o_EnumerateExtensions{};
static inline bool CallerIsStreamline(void*);
static VKAPI_ATTR VkResult VKAPI_CALL GameExtensions(VkPhysicalDevice pd,const char* layer,uint32_t* count,VkExtensionProperties* output){
  if(Cfg().provider!=FGProvider::DLSSG && !IsDX12Bridge(Cfg().provider))return o_EnumerateExtensions(pd,layer,count,output);
  if(ForceReal() || CallerIsStreamline(_ReturnAddress()))return o_EnumerateExtensions(pd,layer,count,output);
  if(!count)return VK_ERROR_INITIALIZATION_FAILED;
  uint32_t nativeCount=0;
  VkResult result=o_EnumerateExtensions(pd,layer,&nativeCount,nullptr);
  if(result!=VK_SUCCESS)return result;
  std::vector<VkExtensionProperties> props(nativeCount);
  result=o_EnumerateExtensions(pd,layer,&nativeCount,props.data());
  if(result!=VK_SUCCESS)return result;
  uint32_t visible=0,capacity=output?*count:0;
  bool hidden=false;
  for(uint32_t i=0;i<nativeCount;i++){
    if(Cfg().provider==FGProvider::DLSSG && !strcmp(props[i].extensionName,VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)){hidden=true;continue;}
    if(IsDX12Bridge(Cfg().provider) && XessHiddenExtension(props[i].extensionName))continue;
    if(output && visible<capacity)output[visible]=props[i];
    ++visible;
  }
  static bool logged=false;if(hidden&&!logged){logged=true;Log("Game Vulkan compatibility: legacy EXT buffer address hidden; SL uses enabled KHR/core path");}
  *count=output?(visible<capacity?visible:capacity):visible;
  return output && capacity<visible?VK_INCOMPLETE:VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
static VKAPI_ATTR VkResult VKAPI_CALL h_CreateDeviceExport(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out){
  // GPU support checks still run when capture is unavailable or simulated.
  if(!ForceReal() && IsDX12Bridge(Cfg().provider) && gInstance){if(!t_CreateDevice)t_CreateDevice=o_CreateDevice_export;return w_CreateDevice(pd,ci,a,out);}
  if(!ForceReal() && ip_GIPA && gInstance){
    if(!t_CreateDevice){Reentry guard;t_CreateDevice=(PFN_vkCreateDevice)ip_GIPA(gInstance,"vkCreateDevice");}
    if(t_CreateDevice)return w_CreateDevice(pd,ci,a,out);
  }
  int prevReentry = g_loaderCreateReentry; g_loaderCreateReentry = g_reentry;
  if(ci){Log("native device forwarding: queueInfos=%u extensions=%u flags=0x%x",ci->queueCreateInfoCount,ci->enabledExtensionCount,ci->flags);
    for(uint32_t i=0;i<ci->queueCreateInfoCount;++i){auto& queue=ci->pQueueCreateInfos[i];Log("native queue[%u]: family=%u count=%u flags=0x%x",i,queue.queueFamilyIndex,queue.queueCount,queue.flags);for(uint32_t j=0;j<i;++j)if(ci->pQueueCreateInfos[j].queueFamilyIndex==queue.queueFamilyIndex && ci->pQueueCreateInfos[j].flags==queue.flags)Log("WARNING: duplicate native queue family=%u flags=0x%x",queue.queueFamilyIndex,queue.flags);}
    for(uint32_t i=0;i<ci->enabledExtensionCount;++i)Log("native device extension[%u]=%s",i,ci->ppEnabledExtensionNames[i]);}
  ++g_loaderCreateDevice; VkResult r = o_CreateDevice_export(pd, ci, a, out); --g_loaderCreateDevice;
  g_loaderCreateReentry = prevReentry;Log("native device forwarding return=%d",(int)r); return r;
}
static void InstallCreateDeviceExportHook(){
  static bool done=false; if(done) return; done=true;
  HMODULE vk = GetModuleHandleA("vulkan-1.dll"); if(!vk) return;
  o_CreateDevice_export = (PFN_vkCreateDevice)GetProcAddress(vk, "vkCreateDevice"); if(!o_CreateDevice_export) return;
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_CreateDevice_export,(PVOID)h_CreateDeviceExport);
  LONG r = DetourTransactionCommit();
  Log("export vkCreateDevice hooked late (outermost) commit=%ld - third-party lookups inside it get the game's view", r);
}

// ---- thin wrappers: call the INTERPOSER target, capture/act, return ----------------------
// slInit runs HERE (in the actual vkCreateInstance CALL), never in the GIPA resolve: the
// resolve runs inside the game's vkGetInstanceProcAddr which holds the Vulkan loader lock, and
// slInit's plugin LoadLibrary -> DllMain needs that same lock = deadlock (the slInit hang, no
// sl.log). The create CALL runs in normal execution, lock released.
static void EnsureSlAndInterposer(){
  // The bridge does not interpose Vulkan. Wait for the game's selected physical
  // device before loading NVIDIA runtimes; adapter enumeration order is not ownership.
  if(Cfg().provider==FGProvider::DLSSGBridge)return;
  static bool inited=false; if(inited) return; inited=true;
  if(!frameGenerationAvailable)return;
  if(Cfg().provider!=FGProvider::DLSSG)return;
  if(!LoadAmpereUnlock()){RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,GPUUnlockStatus());return;}
  Reentry _; GlobalReal _g;   // interposer load + slInit forward to the real loader (all threads)
  ip_GIPA = (PFN_vkGetInstanceProcAddr)SlProxyFn("vkGetInstanceProcAddr");
  ip_GDPA = (PFN_vkGetDeviceProcAddr)SlProxyFn("vkGetDeviceProcAddr");
  if(!EnsureStreamlineInit()){ip_GIPA=nullptr;ip_GDPA=nullptr;Log("Streamline init failed; native Vulkan dispatch");return;}
  Log("SL init in w_CreateInstance: ip_GIPA=%p ip_GDPA=%p", (void*)ip_GIPA,(void*)ip_GDPA);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_CreateInstance(
    const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out){
  if(IsGpuBlocked())return o_CreateInstance_export(ci,a,out);
  if(!g_owner.ClaimInstance()){
    Reentry guard;
    auto r=o_CreateInstance_export?o_CreateInstance_export(ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
    Log("Auxiliary Vulkan instance uses native dispatch: result=%d instance=%p; FG owner preserved",(int)r,r==VK_SUCCESS?(void*)*out:nullptr);
    return r;
  }
  StartDiagnosticCollector();LogModules();
  InstallCreateDeviceExportHook();   // late attach: after other mods' load-time hooks, so ours is outermost
  EnsureSlAndInterposer();   // slInit here (loader lock released), NOT in the GIPA resolve
  // Resolve the interposer's vkCreateInstance now that SL is initialized.
  if(!t_CreateInstance){ Reentry _; t_CreateInstance = (PFN_vkCreateInstance)(ip_GIPA ? ip_GIPA(nullptr,"vkCreateInstance") : nullptr); }
  if(!t_CreateInstance){
    Log("No active interposer; falling back to the Vulkan loader");
    auto nativeCreate=o_CreateInstance_export;
    auto r=nativeCreate?nativeCreate(ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
    if(r==VK_SUCCESS){gInstance=*out;g_ownedInstance.store(*out);}else g_owner.InstanceFailed();return r;
  }
  VkResult r; { Reentry _; r = t_CreateInstance(ci, a, out); }
  if (r == VK_SUCCESS){ gInstance = *out;g_ownedInstance.store(*out); Log("w_CreateInstance ok instance=%p", (void*)gInstance); }
  else g_owner.InstanceFailed();
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL h_CreateInstanceExport(const VkInstanceCreateInfo* ci,const VkAllocationCallbacks* a,VkInstance* out){
  if(ForceReal())return o_CreateInstance_export(ci,a,out);
  return w_CreateInstance(ci,a,out);
}

static bool PhysicalDeviceBelongs(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd){
  if(!gipa || !inst || !pd) return false;
  Reentry guard;
  auto enumerate=(PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
  uint32_t count=0;
  if(!enumerate || enumerate(inst,&count,nullptr)!=VK_SUCCESS || !count || count>64) return false;
  std::vector<VkPhysicalDevice> devices(count);
  auto r=enumerate(inst,&count,devices.data());
  if(r!=VK_SUCCESS && r!=VK_INCOMPLETE) return false;
  for(uint32_t i=0;i<count && i<devices.size();++i) if(devices[i]==pd) return true;
  return false;
}
static void ResolvePacer(VkDevice dev);
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a, VkDevice* out){
  if(IsGpuBlocked())return o_CreateDevice_export?o_CreateDevice_export(pd,ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
  // Physical-device handles belong to an instance. Reserve the game's device
  // before calling the driver so overlapping helper creation stays native.
  auto owner=g_ownedInstance.load();
  bool belongs=PhysicalDeviceBelongs(o_GIPA,owner,pd) || PhysicalDeviceBelongs(ip_GIPA,gInstance,pd);
  if(!belongs || !g_owner.ClaimDevice()){
    Reentry guard;auto r=o_CreateDevice_export?o_CreateDevice_export(pd,ci,a,out):VK_ERROR_INITIALIZATION_FAILED;
    Log("Auxiliary Vulkan device uses native dispatch: physical=%p result=%d device=%p; FG owner preserved",(void*)pd,(int)r,r==VK_SUCCESS?(void*)*out:nullptr);
    return r;
  }
  struct Reservation {bool committed=false;~Reservation(){if(!committed)g_owner.DeviceFailed();}} reservation;
  if(Cfg().simulateUnsupportedAMD){frameGenerationAvailable=false;BlockUnsupportedGpu(true);}
  // Use the actual game Vulkan device, not the first DXGI adapter (which can
  // be an AMD iGPU in a working NVIDIA laptop). No FG context exists yet.
  {Reentry guard;
    auto props=o_GIPA?(PFN_vkGetPhysicalDeviceProperties)o_GIPA(owner,"vkGetPhysicalDeviceProperties"):nullptr;
    if(props){
      VkPhysicalDeviceProperties info{};props(pd,&info);SetGpuInfo(info);
      if(IsUnsupportedGpuVendor(info.vendorID)){
        frameGenerationAvailable=false;BlockUnsupportedGpu(false);
      }
    }
  }
  if(frameGenerationAvailable && Cfg().provider==FGProvider::DLSSGBridge){
    Reentry guard;GlobalReal real;
    // slInit must precede the bridge's D3D12CreateDevice, outside loader lock.
    if(!LoadAmpereUnlock())RuntimeErrors::ReportError(RuntimeErrors::Code::ProviderInit,GPUUnlockStatus());
    else if(!EnsureStreamlineD3D12Init())Log("DLSSG DX12 startup unavailable; bridge will keep native presentation and error menu");
  }
  LogDebug("device create begin: physical=%p queueInfos=%u extensions=%u flags=0x%x",(void*)pd,ci->queueCreateInfoCount,ci->enabledExtensionCount,ci->flags);
  for(uint32_t i=0;i<ci->queueCreateInfoCount;++i){const auto& q=ci->pQueueCreateInfos[i];LogDebug("device requested queue[%u]: family=%u count=%u flags=0x%x",i,q.queueFamilyIndex,q.queueCount,q.flags);}
  // RDR2 can enumerate physical devices via a cached native/export entry point.
  // SL's vkCreateDevice indexes instanceDeviceMap[physicalDevice]; without the
  // intercepted enumeration this inserts a NULL instance and its plugins call
  // a NULL GetPhysicalDeviceMemoryProperties. Prime the association explicitly.
  if(ip_GIPA && gInstance){
    Reentry guard;
    auto enumerate=(PFN_vkEnumeratePhysicalDevices)ip_GIPA(gInstance,"vkEnumeratePhysicalDevices");
    uint32_t count=0;
    bool matched=false;
    if(enumerate && enumerate(gInstance,&count,nullptr)==VK_SUCCESS && count && count<=64){
      std::vector<VkPhysicalDevice> devices(count);
      const auto listed=enumerate(gInstance,&count,devices.data());
      if(listed==VK_SUCCESS || listed==VK_INCOMPLETE)for(uint32_t i=0;i<count && i<devices.size();++i)if(devices[i]==pd)matched=true;
    }
    if(!matched){
      RuntimeErrors::ReportError(RuntimeErrors::Code::VulkanDevice,"Streamline physical-device association unavailable; using native device/menu dispatch");
      Log("SL physical-device association unavailable; FG disabled, native device creation selected");
      frameGenerationAvailable=false;ip_GIPA=nullptr;ip_GDPA=nullptr;t_CreateDevice=o_CreateDevice_export;
    }else Log("SL physical-device association primed: instance=%p physical=%p count=%u",(void*)gInstance,(void*)pd,count);
  }
  // Interposer owns the device (its own surgery/queues). We only capture + kick off SL setup.
  VkDeviceCreateInfo planned{};const VkDeviceCreateInfo* selected=ci;
  if(frameGenerationAvailable && IsDX12Bridge(Cfg().provider) && XessPlanDevice(gInstance,pd,ci,planned,o_GIPA))selected=&planned;
  if(ip_GIPA){ Reentry guard; auto resolved=(PFN_vkCreateDevice)ip_GIPA(gInstance,"vkCreateDevice"); if(resolved) t_CreateDevice=resolved; }
  VkResult r; { Reentry _; r = t_CreateDevice(pd, selected, a, out); }
  if(r!=VK_SUCCESS && selected!=ci){if(IsDX12Bridge(Cfg().provider))frameGenerationAvailable=false;Log("Provider device feature request failed=%d; retrying original native device",r);Reentry guard;r=t_CreateDevice(pd,ci,a,out);}
  if (r == VK_SUCCESS){
    gPhysicalDevice = pd; gDevice = *out;g_ownedDevice.store(*out);reservation.committed=true;
    {auto vk=GetModuleHandleW(L"vulkan-1.dll");auto props=(PFN_vkGetPhysicalDeviceProperties)GetProcAddress(vk,"vkGetPhysicalDeviceProperties");if(props){VkPhysicalDeviceProperties info{};props(pd,&info);SetGpuInfo(info);LogDebug("GPU Vulkan details: vendor=0x%x device=0x%x api=%u.%u.%u driverRaw=0x%x",info.vendorID,info.deviceID,VK_VERSION_MAJOR(info.apiVersion),VK_VERSION_MINOR(info.apiVersion),VK_VERSION_PATCH(info.apiVersion),info.driverVersion);}
    auto queues=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)GetProcAddress(vk,"vkGetPhysicalDeviceQueueFamilyProperties");if(queues){uint32_t count=0;queues(pd,&count,nullptr);if(count && count<=64){std::vector<VkQueueFamilyProperties> families(count);queues(pd,&count,families.data());for(uint32_t i=0;i<count&&i<families.size();++i)LogDebug("GPU available queue family[%u]: count=%u flags=0x%x",i,families[i].queueCount,families[i].queueFlags);}}}
    for (uint32_t i=0;i<ci->queueCreateInfoCount;i++){ gGraphicsFamily = ci->pQueueCreateInfos[i].queueFamilyIndex; break; }
    Log("w_CreateDevice ok device=%p phys=%p gfxFamily=%u", (void*)gDevice,(void*)pd,gGraphicsFamily);
    if(frameGenerationAvailable && IsDX12Bridge(Cfg().provider)){Reentry guard;XessDeviceReady(gInstance,pd,gDevice,gGraphicsFamily,o_GIPA,NativeBackendGDPA);auto selectedFamily=XessGraphicsFamily();if(selectedFamily!=UINT32_MAX)gGraphicsFamily=selectedFamily;}
    if(frameGenerationAvailable && ip_GDPA && Cfg().provider==FGProvider::DLSSG){ResolvePacer(gDevice);Reentry guard;OnDeviceCreated();}
    StartWatchdog();
  }
  return r;
}

static void TrackOwnedSwapchain(VkSwapchainKHR sc);
static void UntrackOwnedSwapchain(VkSwapchainKHR sc);
static PresentRoute RouteForPresent(const VkPresentInfoKHR* pi);
static VKAPI_ATTR VkResult VKAPI_CALL w_CreateSwapchainKHR(
    VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a, VkSwapchainKHR* out){
  if(IsGpuBlocked()){Reentry guard;return o_CreateSwapchainKHR_export(dev,ci,a,out);}
  SwapchainScope lifecycle;
  xess_bridge::AllQueuesGuard queueGuard(IsDX12Bridge(Cfg().provider));
  VkSwapchainCreateInfoKHR stableCreate{};
  if(ci && Cfg().antiTearing){stableCreate=*ci;stableCreate.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci=&stableCreate;Log("Anti-tearing: FIFO presentation selected for swapchain creation");}
  if(frameGenerationAvailable && IsDX12Bridge(Cfg().provider)){Reentry guard;auto r=XessCreateSwapchain(dev,ci,a,out);if(r==VK_SUCCESS){TrackOwnedSwapchain(*out);auto getter=(PFN_vkGetSwapchainImagesKHR)XessOverride("vkGetSwapchainImagesKHR",NativeBackendGDPA(dev,"vkGetSwapchainImagesKHR"));MenuRegisterSwapchain(dev,gPhysicalDevice,gInstance,gGraphicsFamily,*out,ci,XessMenuGDPA,getter);uint32_t count=0;if(getter(dev,*out,&count,nullptr)==VK_SUCCESS && count>=2 && count<=8){std::vector<VkImage> images(count);if(getter(dev,*out,&count,images.data())==VK_SUCCESS)MenuCaptureImages(dev,*out,count,images.data());}}return r;}
  VkSwapchainCreateInfoKHR hudCreate{};
  if(ci && NativeHudEnabled() && !(ci->imageUsage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){
    Reentry guard;auto caps=(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)LoaderFn("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");VkSurfaceCapabilitiesKHR supported{};
    if(caps && caps(gPhysicalDevice,ci->surface,&supported)==VK_SUCCESS && (supported.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)){
      hudCreate=*ci;hudCreate.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci=&hudCreate;
    }
  }
  // GAME'S EXACT swapchain (PureDark parity - his log: 'created original SwapChain'). The
  // earlier 7-image bump likely broke HW flip metering (shallow flip-queue depth limits) ->
  // 'FC feedback' warnings -> CPU-pacer fallback whose 100ms semaphore waits are the freeze.
  // The acquire starvation the bump addressed came from the (now-fixed) marker-mutex era.
  static bool logged=false; if(!logged){ logged=true;
    Log("w_CreateSwapchainKHR %ux%u fmt=%d mode=%d minImg=%u (game's own)",
        ci->imageExtent.width, ci->imageExtent.height, (int)ci->imageFormat,
        (int)ci->presentMode, ci->minImageCount); }
  GateOffForSwapchain();
  VkResult r; { Reentry _; r = t_CreateSwapchainKHR(dev, ci, a, out); }
  if(r==VK_SUCCESS){TrackOwnedSwapchain(*out);Reentry guard;auto resolver=ip_GDPA?ip_GDPA:o_GDPA_real;auto getter=resolver?(PFN_vkGetSwapchainImagesKHR)resolver(dev,"vkGetSwapchainImagesKHR"):nullptr;MenuRegisterSwapchain(dev,gPhysicalDevice,gInstance,gGraphicsFamily,*out,ci,NativeBackendGDPA,getter);NativeHudRegister(dev,gPhysicalDevice,*out,ci,NativeBackendGDPA);}
  static bool l2=false; if(!l2){ l2=true; Log("w_CreateSwapchainKHR -> %d sc=%p", (int)r, out?(void*)*out:nullptr); }
  return r;
}

// PresentPacing (see presentpacer.h): resolved once per device through the interposer, used on
// the present thread only. Stats go to the log every 600 paced frames.
static std::mutex g_ownedSwapchainsMutex;
static std::vector<VkSwapchainKHR> g_ownedSwapchains;
static void TrackOwnedSwapchain(VkSwapchainKHR sc){
  if(!sc) return;
  std::lock_guard<std::mutex> lock(g_ownedSwapchainsMutex);
  if(std::find(g_ownedSwapchains.begin(),g_ownedSwapchains.end(),sc)==g_ownedSwapchains.end())g_ownedSwapchains.push_back(sc);
}
static void UntrackOwnedSwapchain(VkSwapchainKHR sc){
  std::lock_guard<std::mutex> lock(g_ownedSwapchainsMutex);
  g_ownedSwapchains.erase(std::remove(g_ownedSwapchains.begin(),g_ownedSwapchains.end(),sc),g_ownedSwapchains.end());
}
static PresentRoute RouteForPresent(const VkPresentInfoKHR* pi){
  if(!pi) return PresentRoute::FrameGeneration;
  std::lock_guard<std::mutex> lock(g_ownedSwapchainsMutex);
  bool anyKnown=false;
  for(uint32_t i=0;i<pi->swapchainCount;++i)
    if(std::find(g_ownedSwapchains.begin(),g_ownedSwapchains.end(),pi->pSwapchains[i])!=g_ownedSwapchains.end()){anyKnown=true;break;}
  return RoutePresent(g_ownedSwapchains.empty(),anyKnown,pi->swapchainCount);
}
static PacerVk g_pacerVk{};
static PresentPacer g_pacer;
static void ResolvePacer(VkDevice dev){
  if(!ip_GDPA) return;
  Reentry _;
  PacerVk vk{}; vk.device=dev;
  vk.createFence    = (PFN_vkCreateFence)ip_GDPA(dev,"vkCreateFence");
  vk.resetFences    = (PFN_vkResetFences)ip_GDPA(dev,"vkResetFences");
  vk.getFenceStatus = (PFN_vkGetFenceStatus)ip_GDPA(dev,"vkGetFenceStatus");
  vk.waitForFences  = (PFN_vkWaitForFences)ip_GDPA(dev,"vkWaitForFences");
  vk.queueSubmit    = (PFN_vkQueueSubmit)ip_GDPA(dev,"vkQueueSubmit");
  g_pacerVk=vk;
  Log("PresentPacing configured=%d; wait functions=%s",(int)Rt().presentPacing,
      vk.createFence&&vk.resetFences&&vk.getFenceStatus&&vk.waitForFences&&vk.queueSubmit ? "ok" : "MISSING");
}
static void PacePresent(VkQueue q){
  static uint32_t n=0, timeouts=0, skipped=0, unavailable=0; static double sumMs=0, maxMs=0;
  double t0=NowUs()/1000.0; PaceResult r;
  { PosScope _w(9); Reentry _; r = g_pacer.Pace(g_pacerVk, q, 100ull*1000*1000); }
  double ms=NowUs()/1000.0-t0;
  n++; sumMs+=ms; if(ms>maxMs) maxMs=ms;
  if(r==PaceResult::TimedOut) timeouts++; else if(r==PaceResult::SkippedBusy) skipped++; else if(r==PaceResult::Unavailable) unavailable++;
  if(n==600){
    Log("pacing: 600 frames avg wait %.2fms max %.2fms timeouts=%u skipped=%u unavailable=%u", sumMs/n, maxMs, timeouts, skipped, unavailable);
    n=timeouts=skipped=unavailable=0; sumMs=maxMs=0; }
}

static void ApplyFPSCap(){
  // Both bridges apply the same cap at frame start via Reflex or XeLL. A
  // second sleep after rendering competes with each SDK's output pacing.
  if(IsDX12Bridge(Cfg().provider) || (Cfg().provider==FGProvider::DLSSG && Cfg().reflexSleep))return;
  uint32_t cap=Rt().fpsCap.load(std::memory_order_relaxed);
  static uint32_t appliedCap=0;static long long nextDeadline=0;
  if(cap!=appliedCap){appliedCap=cap;nextDeadline=0;}
  long long interval=FrameBudgetUs(cap,Rt().outputFPSCap.load(),g_genOn.load()?Rt().frames+1:1);
  if(!interval)return;
  long long now=NowUs();
  if(!nextDeadline || now<nextDeadline-interval*4 || now>nextDeadline+interval*4)nextDeadline=now;
  nextDeadline+=interval;
  long long wait=nextDeadline-now;
  if(wait>0)Sleep((DWORD)((wait+999)/1000)); // No render-thread spin/yield loop.
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* pi){
  if(IsGpuBlocked()){Reentry guard;return o_QueuePresentKHR_export(q,pi);}
  if(RouteForPresent(pi)==PresentRoute::Native){
    static std::atomic<int> routed{0};
    if(routed.fetch_add(1)<5)Log("present routed native: swapchain %p was not created for the owned device",(pi && pi->swapchainCount && pi->pSwapchains)?(void*)pi->pSwapchains[0]:nullptr);
    xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));
    Reentry guard;
    if(o_QueuePresentKHR_export) return o_QueuePresentKHR_export(q, pi);
    return t_QueuePresentKHR ? t_QueuePresentKHR(q, pi) : VK_ERROR_INITIALIZATION_FAILED;
  }
  ApplyFPSCap();
  static std::atomic<uint32_t> diagnosticPresent{0};
  const uint32_t serial=diagnosticPresent.fetch_add(1,std::memory_order_relaxed)+1;
  const bool trace=DebugLoggingEnabled() && (serial<=256 || serial%120==0);
  if(trace){Log("present[%u] enter: queue=%p waits=%u swapchains=%u",serial,(void*)q,pi?pi->waitSemaphoreCount:0,pi?pi->swapchainCount:0);if(pi){auto node=(const VkBaseInStructure*)pi->pNext;for(unsigned i=0;node && i<16;++i,node=node->pNext)Log("present[%u] pNext[%u] type=%d",serial,i,(int)node->sType);}}
  SwapchainScope lifecycle;
  xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));
  if(IsDX12Bridge(Cfg().provider) && !XessValidatePresentQueue(q,pi))return VK_ERROR_FEATURE_NOT_PRESENT;
  if(trace)Log("present[%u] lifecycle lock acquired",serial);
  if(trace && pi){for(uint32_t i=0;i<pi->waitSemaphoreCount;++i)Log("present[%u] waitSemaphore[%u]=%p",serial,i,(void*)pi->pWaitSemaphores[i]);for(uint32_t i=0;i<pi->swapchainCount;++i)Log("present[%u] swapchain[%u]=%p imageIndex=%u",serial,i,(void*)pi->pSwapchains[i],pi->pImageIndices[i]);}
  StartMenu();
  if(!frameGenerationAvailable || (Cfg().provider==FGProvider::DLSSG && !ip_GDPA)){
    PollHotkeys();Reentry guard;VkPresentInfoKHR overlay{};
    const auto* selected=MenuRender(q,pi,overlay)?&overlay:pi;
    const auto result=t_QueuePresentKHR?t_QueuePresentKHR(q,selected):VK_ERROR_INITIALIZATION_FAILED;
    MenuPresentCompleted(false,result);
    if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)RecordGamePresent();
    return result;
  }
  InputCapturePoll();
  if(DebugLoggingEnabled() && (serial==1 || serial%300==0)){
    const auto reported=RuntimeErrors::SnapshotErrors();
    for(size_t i=0;i<reported.count;++i){const auto& e=reported.entries[i];Log("DIAGNOSTIC %s: %s; native=%d count=%llu detail=%s",e.id,e.title,e.native,(unsigned long long)e.count,e.detail.data());}
    const auto fg=GetFGSnapshot();Log("DIAGNOSTIC provider: requested=%u accepted=%u state=%d options=%d query=%d shown=%u measuredRatio=%.3f warnings=0x%x",(unsigned)fg.requested,(unsigned)fg.accepted,(int)fg.state,fg.optionsResult,fg.queryResult,fg.lastPresented,fg.measuredRatio,RuntimeErrors::GetWarnings());
  }
  StartWatchdog();
  g_wdPresents.fetch_add(1);
  if(!g_presentTid.load()) g_presentTid.store((uint32_t)GetCurrentThreadId());
  PosScope _p(1);
  NgxProbeTick();                  // cheap after hooked (one bool)
  if(frameGenerationAvailable && IsDX12Bridge(Cfg().provider)){PollHotkeys();bool seen=ConsumeEvalSeen();Reentry guard;VkPresentInfoKHR overlay{};auto selected=MenuRender(q,pi,overlay)?&overlay:pi;auto result=XessPresent(q,selected);MenuPresentCompleted(seen,result);if(result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR)RecordGamePresent();return result;}
  ProcessNativeRestart();
  ConsumeEvalSeen();
  // Read readiness from the exact token selected for this present, not from
  // an unrelated evaluation that may have run ahead on another thread.
  const bool inputsReady=PresentMarkersBegin(pi);
  if(trace)Log("present[%u] markers ready=%d; generation=%d",serial,(int)inputsReady,(int)g_genOn.load());
  // No instant off on one unready present; EvalGate suspends after the grace window.
  EvalGate(inputsReady);
  double timing0=NowUs();
  if(Rt().presentPacing && g_genOn.load()) PacePresent(q);   // frame's GPU work done before PresentStart
  double timing1=NowUs();
  if(trace)Log("present[%u] DLSS-G state query begin",serial);
  PollDLSSGState();                // slDLSSGGetState on the present thread: status + generated-frame stats
  if(trace)Log("present[%u] DLSS-G state query end",serial);
  static bool logged=false; if(!logged){ logged=true;
    Log("w_QueuePresentKHR live queue=%p tid=%lu waitSems=%u swapchains=%u pNext=%s (eval-driven gate)", (void*)q,
        (unsigned long)GetCurrentThreadId(), pi?pi->waitSemaphoreCount:0u, pi?pi->swapchainCount:0u, (pi&&pi->pNext)?"yes":"no"); }
  static int oddLogged=0; if(pi && pi->waitSemaphoreCount!=1 && oddLogged<5){ oddLogged++; Log("present with waitSemaphoreCount=%u", pi->waitSemaphoreCount); }
  VkResult r;
  double timing2=NowUs();
  { PosScope _q(2); Reentry _; VkPresentInfoKHR overlay{},compatible{};if(trace)Log("present[%u] overlay begin",serial);bool drew=MenuRender(q,pi,overlay);const auto* selected=drew?&overlay:pi;
    if(NormalizeLocalPresent(selected,compatible)){selected=&compatible;static bool noted=false;if(!noted){noted=true;Log("Vulkan present compatibility: redundant LOCAL device-group mask 1 removed; remaining extensions preserved");}}
    if(trace)Log("present[%u] overlay end drew=%d; Streamline present begin",serial,(int)drew);
    if(serial==1)Log("Diagnostic first-presentation checkpoint before Streamline call");
    r = t_QueuePresentKHR(q, selected);if(trace)Log("present[%u] Streamline present end result=%d",serial,(int)r); }   // interposer present = DLSS-G generation
  double timing3=NowUs();
  MenuPresentCompleted(inputsReady,r);
  static int badLogged=0; if(r!=VK_SUCCESS && badLogged<10){ badLogged++; Log("present -> %d", (int)r); }
  if(r!=VK_SUCCESS && r!=VK_SUBOPTIMAL_KHR && r!=VK_ERROR_OUT_OF_DATE_KHR){char detail[128];snprintf(detail,sizeof(detail),"vkQueuePresentKHR failed on the DLSS-G present path; serial=%u",serial);RuntimeErrors::ReportError(RuntimeErrors::Code::PresentFailed,detail,(int)r);}
  else if(r==VK_SUCCESS || r==VK_SUBOPTIMAL_KHR)RuntimeErrors::ClearError(RuntimeErrors::Code::PresentFailed);
  { PosScope _m(3); PresentMarkersEnd(r); }   // PresentEnd, then next frame: new token + Reflex sleep + SimulationStart
  nativeRestart.Presented(r==VK_SUCCESS || r==VK_SUBOPTIMAL_KHR);
  if(trace)Log("present[%u] completed",serial);
  if(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR)RecordGamePresent();
  if(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR)PerformanceProbeFrame((timing1-timing0)/1000.0,(timing3-timing2)/1000.0,(timing2-timing1+NowUs()-timing3)/1000.0);
  return r;
}

// ---- stall attribution: thin pass-through wrappers on every call the game can block in ------
static PFN_vkAcquireNextImageKHR t_AcquireNextImageKHR{};
static PFN_vkAcquireNextImage2KHR t_AcquireNextImage2KHR{};
static PFN_vkGetSwapchainImagesKHR t_GetSwapchainImagesKHR{};
static VKAPI_ATTR VkResult VKAPI_CALL w_GetSwapchainImagesKHR(VkDevice d,VkSwapchainKHR sc,uint32_t* count,VkImage* images){
  if(IsGpuBlocked()){Reentry guard;return o_GetSwapchainImagesKHR_export(d,sc,count,images);}
  uint32_t capacity=images&&count?*count:0;
  VkResult result;{Reentry guard;result=t_GetSwapchainImagesKHR(d,sc,count,images);}
  if(images && count && (result==VK_SUCCESS || result==VK_INCOMPLETE))MenuCaptureImages(d,sc,*count,images);
  if(images && count && result==VK_SUCCESS)NativeHudImages(d,sc,*count,images);
  static int logged=0;if(logged++<6)Log("Game swapchain images: capacity=%u returned=%u result=%d images=%s",capacity,count?*count:0,(int)result,images?"yes":"no");
  return result;
}
static PFN_vkWaitForFences       t_WaitForFences{};
static PFN_vkQueueSubmit         t_QueueSubmit{};
static PFN_vkQueueSubmit2        t_QueueSubmit2{};      // vkQueueSubmit2 and vkQueueSubmit2KHR share the prototype
static PFN_vkDeviceWaitIdle      t_DeviceWaitIdle{};
static PFN_vkQueueWaitIdle       t_QueueWaitIdle{};
static PFN_vkSetHdrMetadataEXT   t_SetHdrMetadataEXT{};
static VKAPI_ATTR VkResult VKAPI_CALL w_AcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t* idx){
  static bool logged=false; if(!logged){ logged=true; Log("game acquire: swapchain=%p timeout=%llu semaphore=%p fence=%p", (void*)sc, (unsigned long long)timeout, (void*)sem, (void*)fence); }
  VkResult r; { PosScope _(4); Reentry _r; r = t_AcquireNextImageKHR(d,sc,timeout,sem,fence,idx); }
  if((r==VK_SUCCESS || r==VK_SUBOPTIMAL_KHR) && idx)NativeHudAcquire(sc,*idx);
  static int n=0; if(r!=VK_SUCCESS && n<10){ n++; Log("game acquire -> %d (index=%u)", (int)r, idx?*idx:0u); }
  return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_AcquireNextImage2KHR(VkDevice d,const VkAcquireNextImageInfoKHR* info,uint32_t* idx){
  if(IsDX12Bridge(Cfg().provider)){PosScope position(4);Reentry guard;return t_AcquireNextImage2KHR?t_AcquireNextImage2KHR(d,info,idx):VK_ERROR_INITIALIZATION_FAILED;}
  if(!info || info->deviceMask!=1){Reentry guard;return t_AcquireNextImage2KHR(d,info,idx);}
  {Reentry guard;t_AcquireNextImageKHR=(PFN_vkAcquireNextImageKHR)ip_GDPA(d,"vkAcquireNextImageKHR");}
  static bool logged=false;if(!logged){logged=true;Log("Game acquire2 bridged to Streamline acquire1; deviceMask=1");}
  return w_AcquireNextImageKHR(d,info->swapchain,info->timeout,info->semaphore,info->fence,idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_WaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t timeout){
  PosScope _(5); return t_WaitForFences(d,n,f,all,timeout);
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f){
  if(IsGpuBlocked()){Reentry guard;return t_QueueSubmit(q,n,s,f);}
  xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));
  if(IsDX12Bridge(Cfg().provider))for(uint32_t i=0;i<n;++i)XessBeforeSubmit(q,s[i].commandBufferCount,s[i].pCommandBuffers);
  PosScope _(6); VkResult r;{Reentry guard;r=t_QueueSubmit(q,n,s,f);}if(r==VK_SUCCESS)RecordFGSubmission(q,n,s);if(r==VK_SUCCESS)for(uint32_t i=0;i<n;++i){InputCaptureSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);RenderReprojectionSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);CommitCommands(s[i].commandBufferCount,s[i].pCommandBuffers);if(IsDX12Bridge(Cfg().provider)){HudCaptureOnSubmit(s[i].commandBufferCount,s[i].pCommandBuffers);XessSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);}}return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f){
  if(IsGpuBlocked()){Reentry guard;return t_QueueSubmit2(q,n,s,f);}
  xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));
  if(IsDX12Bridge(Cfg().provider))for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<s[i].commandBufferInfoCount;++j){auto c=s[i].pCommandBufferInfos[j].commandBuffer;XessBeforeSubmit(q,1,&c);}
  PosScope _(6);VkResult r;{Reentry guard;r=t_QueueSubmit2(q,n,s,f);}if(r==VK_SUCCESS)RecordFGSubmission2(q,n,s);if(r==VK_SUCCESS)for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<s[i].commandBufferInfoCount;++j){auto c=s[i].pCommandBufferInfos[j].commandBuffer;InputCaptureSubmitted(q,1,&c);RenderReprojectionSubmitted(q,1,&c);CommitCommands(1,&c);if(IsDX12Bridge(Cfg().provider)){HudCaptureOnSubmit(1,&c);XessSubmitted(q,1,&c);}}return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_DeviceWaitIdle(VkDevice d){
  xess_bridge::AllQueuesGuard queueGuard(IsDX12Bridge(Cfg().provider));
  Log("game vkDeviceWaitIdle (tid=%lu)", (unsigned long)GetCurrentThreadId());
  VkResult r; { PosScope _(7); Reentry _r; r = t_DeviceWaitIdle(d); } return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL w_QueueWaitIdle(VkQueue q){
  xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));
  static int n=0; if(n<5){ n++; Log("game vkQueueWaitIdle queue=%p (tid=%lu)", (void*)q, (unsigned long)GetCurrentThreadId()); }
  PosScope _(8); return t_QueueWaitIdle(q);
}
static VKAPI_ATTR void VKAPI_CALL w_SetHdrMetadataEXT(VkDevice d, uint32_t n, const VkSwapchainKHR* sc, const VkHdrMetadataEXT* md){
  Log("game vkSetHdrMetadataEXT swapchains=%u first=%p maxLum=%.1f (tid=%lu)", n, (n&&sc)?(void*)sc[0]:nullptr, md?md->maxLuminance:0.f, (unsigned long)GetCurrentThreadId());
  t_SetHdrMetadataEXT(d,n,sc,md);
}
// Swap in a wrapper for the names above; everything else passes straight through.
static PFN_vkDestroySwapchainKHR t_DestroySwapchainKHR{};
static VKAPI_ATTR void VKAPI_CALL w_DestroySwapchainKHR(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks* a){
  if(IsGpuBlocked()){Reentry guard;return o_DestroySwapchainKHR_export(d,sc,a);}
  SwapchainScope lifecycle;
  xess_bridge::AllQueuesGuard queueGuard(IsDX12Bridge(Cfg().provider));
  if(Cfg().provider==FGProvider::DLSSG)GateOffForSwapchain();
  UntrackOwnedSwapchain(sc);
  Reentry guard;MenuDestroySwapchain(d,sc);t_DestroySwapchainKHR(d,sc,a);
  // The provider must flush its asynchronous consumers before the tagged
  // snapshots are freed. These images are independent of the swapchain.
  NativeHudDestroy(d,sc);
}
static PFN_vkVoidFunction WrapDeviceFn(const char* name, PFN_vkVoidFunction ip){
  if(IsDX12Bridge(Cfg().provider))ip=XessOverride(name,ip);
  if(!strcmp(name,"vkDestroySwapchainKHR")){t_DestroySwapchainKHR=(PFN_vkDestroySwapchainKHR)ip;return (PFN_vkVoidFunction)w_DestroySwapchainKHR;}
  if(!strcmp(name,"vkGetSwapchainImagesKHR")){t_GetSwapchainImagesKHR=(PFN_vkGetSwapchainImagesKHR)ip;return (PFN_vkVoidFunction)w_GetSwapchainImagesKHR;}
  if(!strcmp(name,"vkAcquireNextImage2KHR")){t_AcquireNextImage2KHR=(PFN_vkAcquireNextImage2KHR)ip;return (PFN_vkVoidFunction)w_AcquireNextImage2KHR;}
  if(!strcmp(name,"vkAcquireNextImageKHR")){ t_AcquireNextImageKHR=(PFN_vkAcquireNextImageKHR)ip; return (PFN_vkVoidFunction)w_AcquireNextImageKHR; }
  if(!strcmp(name,"vkWaitForFences"))      { t_WaitForFences=(PFN_vkWaitForFences)ip;             return (PFN_vkVoidFunction)w_WaitForFences; }
  if(!strcmp(name,"vkQueueSubmit"))        { t_QueueSubmit=(PFN_vkQueueSubmit)ip;                 return (PFN_vkVoidFunction)w_QueueSubmit; }
  if(!strcmp(name,"vkQueueSubmit2") || !strcmp(name,"vkQueueSubmit2KHR")){ t_QueueSubmit2=(PFN_vkQueueSubmit2)ip; return (PFN_vkVoidFunction)w_QueueSubmit2; }
  if(!strcmp(name,"vkDeviceWaitIdle"))     { t_DeviceWaitIdle=(PFN_vkDeviceWaitIdle)ip;           return (PFN_vkVoidFunction)w_DeviceWaitIdle; }
  if(!strcmp(name,"vkQueueWaitIdle"))      { t_QueueWaitIdle=(PFN_vkQueueWaitIdle)ip;             return (PFN_vkVoidFunction)w_QueueWaitIdle; }
  if(!strcmp(name,"vkSetHdrMetadataEXT"))  { t_SetHdrMetadataEXT=(PFN_vkSetHdrMetadataEXT)ip;     return (PFN_vkVoidFunction)w_SetHdrMetadataEXT; }
  auto wrapped=WrapResourceFn(name,ip);
  return IsDX12Bridge(Cfg().provider)?HudCaptureWrap(name,wrapped):NativeHudWrap(name,wrapped);
}

// ---- wrapped device-proc-addr: hand the game interposer device fns, wrap present/swapchain --
   // real loader GDPA (Detours trampoline once the export is hooked)
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL NativeBackendGDPA(VkDevice d,const char* name){if(IsDX12Bridge(Cfg().provider)){auto raw=XessNativeExport(name);if(raw)return raw;}return o_GDPA_real?o_GDPA_real(d,name):nullptr;}
static PFN_vkVoidFunction GameViewGDPA(VkDevice dev, const char* name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL w_GetDeviceProcAddr(VkDevice dev, const char* name){
  if (!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): go to the real loader, not the interposer.
  if (ForceReal()) return o_GDPA_real ? o_GDPA_real(dev, name) : nullptr;
  return GameViewGDPA(dev, name);
}
static PFN_vkVoidFunction GameViewGDPA(VkDevice dev, const char* name){
  if(IsDX12Bridge(Cfg().provider) && !strcmp(name,"vkGetDeviceProcAddr"))return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  if(dev && dev!=g_ownedDevice.load())return o_GDPA_real?o_GDPA_real(dev,name):nullptr;
  // Before the interposer exists (early third-party resolution) there is nothing to wrap yet.
  if (!ip_GDPA){auto native=o_GDPA_real?o_GDPA_real(dev,name):nullptr;if(!native)return nullptr;
    if(!strcmp(name,"vkCreateSwapchainKHR")){t_CreateSwapchainKHR=(PFN_vkCreateSwapchainKHR)native;return (PFN_vkVoidFunction)w_CreateSwapchainKHR;}
    if(!strcmp(name,"vkQueuePresentKHR")){t_QueuePresentKHR=(PFN_vkQueuePresentKHR)native;return (PFN_vkVoidFunction)w_QueuePresentKHR;}
    return WrapDeviceFn(name,native);
  }
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;   // keep later lookups in our view
  PFN_vkVoidFunction ip; { Reentry _; ip = ip_GDPA(dev, name); }
  if (!ip){LogDebug("Vulkan device entry unavailable: %s device=%p",name,(void*)dev);return nullptr;}
  if (!strcmp(name, "vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }   // wrap: PollDLSSGState MUST be on the present thread
  if (!strcmp(name, "vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return WrapDeviceFn(name, ip);   // stall-attribution wrappers, else the interposer's own device function
}

// ---- the single entry hook: vkGetInstanceProcAddr ---------------------------------------
// Init is DEFERRED to the vkCreateInstance resolution, NOT the first GIPA call: the game's
// first GIPA resolution happens in early loader-lock context, where slInit (which LoadLibrary's
// plugins) deadlocks (observed: log stops right after 'resolved slInit=...', game hangs).
// PureDark inits around instance creation for the same reason. We resolve the interposer's
// GIPA/GDPA FIRST so slInit's own re-entrant GIPA calls see the interposer, not the raw loader.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetInstanceProcAddr(VkInstance inst, const char* name){
  if(!name) return nullptr;
  // Re-entrant (interposer forwarding to the driver): straight to the real loader.
  if(ForceReal() || CallerIsStreamline(_ReturnAddress())) return o_GIPA ? o_GIPA(inst, name) : nullptr;

  // vkCreateInstance: return our wrapper WITHOUT initializing SL here (this runs under the
  // loader lock). slInit + interposer resolution happen inside w_CreateInstance (the CALL).
  if(!strcmp(name,"vkCreateInstance")) return (PFN_vkVoidFunction)w_CreateInstance;
  if(inst && inst!=g_ownedInstance.load())return o_GIPA?o_GIPA(inst,name):nullptr;
  if(!strcmp(name,"vkEnumerateDeviceExtensionProperties"))return (PFN_vkVoidFunction)GameExtensions;

  // Everything else: interposer if ready, else the real loader. Before slInit (ip_GIPA null),
  // the game's dispatch build goes to the real loader - correct, we only need the interposer
  // for create/present, which we swap in below once ready.
  PFN_vkVoidFunction ip = nullptr;
  if(ip_GIPA){ Reentry _; ip = ip_GIPA(inst, name); }
  if(!ip) ip = o_GIPA ? o_GIPA(inst, name) : nullptr;
  if(!ip) return nullptr;
  if(IsDX12Bridge(Cfg().provider) && !strcmp(name,"vkCreateWin32SurfaceKHR"))return XessInstanceFn(name,o_CreateWin32SurfaceKHR_export?(PFN_vkVoidFunction)o_CreateWin32SurfaceKHR_export:ip);
  if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)h_GetInstanceProcAddr;
  if(!strcmp(name,"vkCreateDevice"))    { t_CreateDevice   = (PFN_vkCreateDevice)ip;   return (PFN_vkVoidFunction)w_CreateDevice; }
  if(!strcmp(name,"vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)w_GetDeviceProcAddr;
  if(!strcmp(name,"vkQueuePresentKHR"))   { t_QueuePresentKHR   = (PFN_vkQueuePresentKHR)ip;   return (PFN_vkVoidFunction)w_QueuePresentKHR; }
  if(!strcmp(name,"vkCreateSwapchainKHR")){ t_CreateSwapchainKHR= (PFN_vkCreateSwapchainKHR)ip; return (PFN_vkVoidFunction)w_CreateSwapchainKHR; }
  return WrapDeviceFn(name, ip);   // same wrappers if the game resolves device functions through GIPA
}

// ---- Vulkan function access for inputs.cpp (UI image, mem props) -------------------------
void* DeviceFn(const char* name){ if(IsDX12Bridge(Cfg().provider))return (void*)NativeBackendGDPA(gDevice,name);return (ip_GDPA && gDevice) ? (void*)ip_GDPA(gDevice, name) : nullptr; }
void* LoaderFn(const char* name){
  // instance-scope functions (e.g. vkGetPhysicalDeviceMemoryProperties) via the interposer GIPA
  if(IsDX12Bridge(Cfg().provider))return o_GIPA?(void*)o_GIPA(gInstance,name):nullptr;
  return (ip_GIPA && gInstance) ? (void*)ip_GIPA(gInstance, name) : nullptr;
}

// ---- exported-entry detours: third parties see the game's view ----------------------------
// The game resolves everything through vkGetInstanceProcAddr (hooked above), but overlays and
// the Script Extender link vulkan-1.lib and call the EXPORTS: vkGetDeviceProcAddr hands them
// the driver's entry points (which they then Detour - putting their code, locks included, on
// Streamline's pacer thread: the 22:18 deadlock), and vkGetSwapchainImagesKHR hands them the
// REAL swapchain images while the game renders into Streamline's fake buffers (their overlay
// never shows). Routing the exports through the same wrappers the game gets fixes both: their
// detours land on our wrappers on the game thread, their image queries return the buffers the
// game actually presents. Calls from inside the interposer (ForceReal) still reach the loader.
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL h_GetDeviceProcAddrExport(VkDevice dev, const char* name){
  if (!name) return nullptr;
  if(CallerIsStreamline(_ReturnAddress()))return o_GDPA_real?o_GDPA_real(dev,name):nullptr;
  bool inWindow = InLoaderCreateWindow(g_loaderCreateDevice, g_reentry, g_loaderCreateReentry);
  if (ExportRouting(ForceReal(), inWindow) == ExportView::Real) return o_GDPA_real ? o_GDPA_real(dev, name) : nullptr;
  if (!inWindow) return w_GetDeviceProcAddr(dev, name);
  // A third party resolving inside the loader's vkCreateDevice (its own export detour): game view.
  PFN_vkVoidFunction ip = GameViewGDPA(dev, name);
  static int logged=0; if(logged<8){ logged++;
    Log("export vkGetDeviceProcAddr(%s) inside loader vkCreateDevice (third-party hook) -> game view %p", name, (void*)ip); }
  return ip ? ip : (o_GDPA_real ? o_GDPA_real(dev, name) : nullptr);
}
static PFN_vkAcquireNextImageKHR   o_AcquireNextImageKHR_export{};
static PFN_vkAcquireNextImage2KHR  o_AcquireNextImage2KHR_export{};
// Resolve the game-view function once per name via our own GDPA wrapper (interposer + wrappers).
template <typename F> static F GameView(const char* name, F& cache, VkDevice dev){
  if(dev!=g_ownedDevice.load())return nullptr;
  if(!cache && gDevice){ cache = (F)w_GetDeviceProcAddr(dev, name); }
  return cache;
}
// Streamline's own device table aliases these detours on Wine: vulkan-1 there forwards into
// winevulkan, which serves that table from the SAME addresses it exports. sl.common's
// vkGetSwapchainImagesKHR - issued from inside DLSS-G's swapchain clone - therefore arrives
// here, takes the game's view, and re-enters the clone, which reads a proxy vector it has not
// filled yet: an empty vector, a NULL fallback, and a faulting read (sl.dlss_g+0x55C35).
// ForceReal cannot see it - the game resolves through GIPA and never enters these hooks, so no
// re-entry scope is ever pushed. The Khronos loader keeps lookup and dispatch apart, which is
// why this never reproduces on Windows.
static inline bool CallerIsStreamline(void* ra){
  HMODULE m{}; char path[MAX_PATH]{};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)ra,&m);
  GetModuleFileNameA(m, path, MAX_PATH);
  const char* base=strrchr(path,'\\');base=base?base+1:path;
  return IsStreamlineModule(path) || _stricmp(base,"amd_fidelityfx_vk.dll")==0;
}
static VKAPI_ATTR VkResult VKAPI_CALL h_GetSwapchainImagesKHRExport(VkDevice d, VkSwapchainKHR sc, uint32_t* n, VkImage* imgs){
  static PFN_vkGetSwapchainImagesKHR gv{}; PFN_vkGetSwapchainImagesKHR f = (ForceReal() || CallerIsStreamline(_ReturnAddress())) ? nullptr : GameView("vkGetSwapchainImagesKHR", gv, d);
  static bool logged=false; if(f && !logged){ logged=true; Log("export vkGetSwapchainImagesKHR from tid=%lu routed to the game's (fake-buffer) view", (unsigned long)GetCurrentThreadId()); }
  return f ? f(d,sc,n,imgs) : o_GetSwapchainImagesKHR_export(d,sc,n,imgs);
}
static VKAPI_ATTR VkResult VKAPI_CALL h_AcquireNextImageKHRExport(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore s, VkFence fe, uint32_t* idx){
  static PFN_vkAcquireNextImageKHR gv{}; PFN_vkAcquireNextImageKHR f = (ForceReal() || CallerIsStreamline(_ReturnAddress())) ? nullptr : GameView("vkAcquireNextImageKHR", gv, d);
  return f ? f(d,sc,t,s,fe,idx) : o_AcquireNextImageKHR_export(d,sc,t,s,fe,idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL h_AcquireNextImage2KHRExport(VkDevice d,const VkAcquireNextImageInfoKHR* info,uint32_t* idx){
  static PFN_vkAcquireNextImage2KHR gv{};
  auto f=(ForceReal() || !gDevice || CallerIsStreamline(_ReturnAddress()))?nullptr:GameView("vkAcquireNextImage2KHR",gv,d);
  return f?f(d,info,idx):o_AcquireNextImage2KHR_export(d,info,idx);
}
static VKAPI_ATTR VkResult VKAPI_CALL h_QueuePresentKHRExport(VkQueue q, const VkPresentInfoKHR* pi){
  static PFN_vkQueuePresentKHR gv{}; PFN_vkQueuePresentKHR f = (ForceReal() || !gDevice || CallerIsStreamline(_ReturnAddress())) ? nullptr : GameView("vkQueuePresentKHR", gv, gDevice);
  return f ? f(q,pi) : o_QueuePresentKHR_export(q,pi);
}
static VKAPI_ATTR VkResult VKAPI_CALL h_CreateSwapchainKHRExport(VkDevice d,const VkSwapchainCreateInfoKHR* ci,const VkAllocationCallbacks* a,VkSwapchainKHR* out){
  static PFN_vkCreateSwapchainKHR gv{};
  auto f=(ForceReal() || !gDevice || CallerIsStreamline(_ReturnAddress()))?nullptr:GameView("vkCreateSwapchainKHR",gv,d);
  return f?f(d,ci,a,out):o_CreateSwapchainKHR_export(d,ci,a,out);
}

// RDR2 also calls loader exports directly. Keep independent trampolines:
// a proc-address wrapper can pass through these exports without recursion.
static PFN_vkCreateImage rawCreateImage{};
static PFN_vkDestroyImage rawDestroyImage{};
static PFN_vkCmdPipelineBarrier rawBarrier{};
static PFN_vkBeginCommandBuffer rawBegin{};
static PFN_vkResetCommandBuffer rawReset{};
static PFN_vkQueueSubmit rawSubmit{};
VkResult SubmitInternalReadbackFence(VkQueue queue,VkFence fence){
  if(!rawSubmit)return VK_ERROR_INITIALIZATION_FAILED;
  Reentry guard;VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};return rawSubmit(queue,1,&empty,fence);
}
static PFN_vkQueueSubmit2 rawSubmit2{};
static PFN_vkCmdExecuteCommands rawExecute{};
static PFN_vkCreateCommandPool rawCreatePool{};
static PFN_vkDestroyCommandPool rawDestroyPool{};
static PFN_vkResetCommandPool rawResetPool{};
static PFN_vkAllocateCommandBuffers rawAllocateCommands{};
static PFN_vkFreeCommandBuffers rawFreeCommands{};
static PFN_vkVoidFunction XessNativeExport(const char* name){
#define RAW(n,p) if(!strcmp(name,n))return (PFN_vkVoidFunction)p
 RAW("vkCreateCommandPool",rawCreatePool);RAW("vkDestroyCommandPool",rawDestroyPool);RAW("vkResetCommandPool",rawResetPool);
 RAW("vkAllocateCommandBuffers",rawAllocateCommands);RAW("vkFreeCommandBuffers",rawFreeCommands);
 RAW("vkCreateImage",rawCreateImage);RAW("vkDestroyImage",rawDestroyImage);RAW("vkCmdPipelineBarrier",rawBarrier);
 RAW("vkBeginCommandBuffer",rawBegin);RAW("vkResetCommandBuffer",rawReset);RAW("vkQueueSubmit",rawSubmit);
 RAW("vkQueueSubmit2",rawSubmit2);RAW("vkQueueSubmit2KHR",rawSubmit2);RAW("vkCmdExecuteCommands",rawExecute);
 RAW("vkGetSwapchainImagesKHR",o_GetSwapchainImagesKHR_export);RAW("vkAcquireNextImageKHR",o_AcquireNextImageKHR_export);
 RAW("vkAcquireNextImage2KHR",o_AcquireNextImage2KHR_export);RAW("vkQueuePresentKHR",o_QueuePresentKHR_export);
 RAW("vkCreateSwapchainKHR",o_CreateSwapchainKHR_export);RAW("vkDestroySwapchainKHR",o_DestroySwapchainKHR_export);
#undef RAW
 return nullptr;
}
static VKAPI_ATTR VkResult VKAPI_CALL h_SurfaceExport(VkInstance i,const VkWin32SurfaceCreateInfoKHR* c,const VkAllocationCallbacks* a,VkSurfaceKHR* o){
 if(IsGpuBlocked())return o_CreateWin32SurfaceKHR_export(i,c,a,o);
 auto fn=(PFN_vkCreateWin32SurfaceKHR)XessInstanceFn("vkCreateWin32SurfaceKHR",(PFN_vkVoidFunction)o_CreateWin32SurfaceKHR_export);return fn(i,c,a,o);
}
static VKAPI_ATTR void VKAPI_CALL h_DestroySwapchainExport(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks* a){
 if(ForceReal()){o_DestroySwapchainKHR_export(d,sc,a);return;}
 auto fn=(PFN_vkDestroySwapchainKHR)GameViewGDPA(d,"vkDestroySwapchainKHR");if(fn)fn(d,sc,a);
}
static void VKAPI_CALL CaptureExecute(VkCommandBuffer c,uint32_t n,const VkCommandBuffer* secondary){rawExecute(c,n,secondary);if(!IsGpuBlocked())ExecuteFGCommands(c,n,secondary);}
static VkResult VKAPI_CALL CaptureSubmit(VkQueue q,uint32_t n,const VkSubmitInfo* s,VkFence f){if(IsGpuBlocked())return rawSubmit(q,n,s,f);xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));if(IsDX12Bridge(Cfg().provider))for(uint32_t i=0;i<n;++i)XessBeforeSubmit(q,s[i].commandBufferCount,s[i].pCommandBuffers);auto r=rawSubmit(q,n,s,f);if(r==VK_SUCCESS && !ForceReal())RecordFGSubmission(q,n,s);if(r==VK_SUCCESS)for(uint32_t i=0;i<n;++i){InputCaptureSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);RenderReprojectionSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);CommitCommands(s[i].commandBufferCount,s[i].pCommandBuffers);if(IsDX12Bridge(Cfg().provider)){HudCaptureOnSubmit(s[i].commandBufferCount,s[i].pCommandBuffers);XessSubmitted(q,s[i].commandBufferCount,s[i].pCommandBuffers);}}return r;}
static VkResult VKAPI_CALL CaptureSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2* s,VkFence f){if(IsGpuBlocked())return rawSubmit2(q,n,s,f);xess_bridge::QueueGuard queueGuard(q,IsDX12Bridge(Cfg().provider));if(IsDX12Bridge(Cfg().provider))for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<s[i].commandBufferInfoCount;++j){auto c=s[i].pCommandBufferInfos[j].commandBuffer;XessBeforeSubmit(q,1,&c);}auto r=rawSubmit2(q,n,s,f);if(r==VK_SUCCESS && !ForceReal())RecordFGSubmission2(q,n,s);if(r==VK_SUCCESS)for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<s[i].commandBufferInfoCount;++j){auto c=s[i].pCommandBufferInfos[j].commandBuffer;InputCaptureSubmitted(q,1,&c);RenderReprojectionSubmitted(q,1,&c);CommitCommands(1,&c);if(IsDX12Bridge(Cfg().provider)){HudCaptureOnSubmit(1,&c);XessSubmitted(q,1,&c);}}return r;}
static VkResult VKAPI_CALL CaptureCreatePool(VkDevice d,const VkCommandPoolCreateInfo* c,const VkAllocationCallbacks* a,VkCommandPool* o){
 auto fn=!ForceReal()?(PFN_vkCreateCommandPool)XessOverride("vkCreateCommandPool",(PFN_vkVoidFunction)rawCreatePool):rawCreatePool;return fn(d,c,a,o);
}
static void VKAPI_CALL CaptureDestroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a){
 auto fn=!ForceReal()?(PFN_vkDestroyCommandPool)XessOverride("vkDestroyCommandPool",(PFN_vkVoidFunction)rawDestroyPool):rawDestroyPool;fn(d,p,a);
}
static VkResult VKAPI_CALL CaptureResetPool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f){
 auto fn=!ForceReal()?(PFN_vkResetCommandPool)XessOverride("vkResetCommandPool",(PFN_vkVoidFunction)rawResetPool):rawResetPool;return fn(d,p,f);
}
static VkResult VKAPI_CALL CaptureAllocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* c,VkCommandBuffer* o){
 auto fn=!ForceReal()?(PFN_vkAllocateCommandBuffers)XessOverride("vkAllocateCommandBuffers",(PFN_vkVoidFunction)rawAllocateCommands):rawAllocateCommands;return fn(d,c,o);
}
static void VKAPI_CALL CaptureFreeCommands(VkDevice d,VkCommandPool p,uint32_t n,const VkCommandBuffer* c){
 auto fn=!ForceReal()?(PFN_vkFreeCommandBuffers)XessOverride("vkFreeCommandBuffers",(PFN_vkVoidFunction)rawFreeCommands):rawFreeCommands;fn(d,p,n,c);
}
static VkResult VKAPI_CALL CaptureCreateImage(VkDevice d,const VkImageCreateInfo* ci,const VkAllocationCallbacks* a,VkImage* out){
  if(IsGpuBlocked())return rawCreateImage(d,ci,a,out);
  if(IsDX12Bridge(Cfg().provider) && !ForceReal()){auto fn=(PFN_vkCreateImage)XessOverride("vkCreateImage",(PFN_vkVoidFunction)rawCreateImage);if(fn!=rawCreateImage)return fn(d,ci,a,out);}
  auto r=rawCreateImage(d,ci,a,out);
  if(r==VK_SUCCESS && ci && out){std::lock_guard<std::mutex> lock(resource_detail::mutex);resource_detail::images[*out]={ci->initialLayout,ci->usage,ci->format,ci->extent.width,ci->extent.height};}
  return r;
}
static void VKAPI_CALL CaptureDestroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a){
  if(IsGpuBlocked())return rawDestroyImage(d,image,a);
  {std::lock_guard<std::mutex> lock(resource_detail::mutex);resource_detail::images.erase(image);resource_detail::submitted.erase(image);for(auto& p:resource_detail::commands)p.second.erase(image);}
  rawDestroyImage(d,image,a);
}
static void VKAPI_CALL CaptureBarrier(VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,uint32_t nm,const VkMemoryBarrier* m,uint32_t nb,const VkBufferMemoryBarrier* b,uint32_t ni,const VkImageMemoryBarrier* images){
  if(IsGpuBlocked())return rawBarrier(c,src,dst,dep,nm,m,nb,b,ni,images);
  if(IsDX12Bridge(Cfg().provider) && !ForceReal()){auto fn=(PFN_vkCmdPipelineBarrier)XessOverride("vkCmdPipelineBarrier",(PFN_vkVoidFunction)rawBarrier);if(fn!=rawBarrier){fn(c,src,dst,dep,nm,m,nb,b,ni,images);for(uint32_t i=0;i<ni;++i)ObserveBarrier(c,images[i].image,images[i].newLayout,images[i].subresourceRange);return;}}
  rawBarrier(c,src,dst,dep,nm,m,nb,b,ni,images);
  for(uint32_t i=0;i<ni;++i)ObserveBarrier(c,images[i].image,images[i].newLayout,images[i].subresourceRange);
}
static VkResult VKAPI_CALL CaptureBegin(VkCommandBuffer c,const VkCommandBufferBeginInfo* bi){if(!IsGpuBlocked()){ForgetCommand(c);ForgetFGCommand(c);HudCaptureForgetCommand(c);}return rawBegin(c,bi);}
static VkResult VKAPI_CALL CaptureReset(VkCommandBuffer c,VkCommandBufferResetFlags flags){if(!IsGpuBlocked()){ForgetCommand(c);ForgetFGCommand(c);HudCaptureForgetCommand(c);}return rawReset(c,flags);}
bool InstallVkHooks(bool available){
  frameGenerationAvailable=available;
  HMODULE vk = GetModuleHandleA("vulkan-1.dll");
  if(!vk) vk = LoadLibraryA("vulkan-1.dll");
  if(!vk){RuntimeErrors::ReportError(RuntimeErrors::Code::HookInstall,"vulkan-1.dll could not be loaded",(int)GetLastError());Log("InstallVkHooks: vulkan-1.dll not found");return false;}
  rawCreateImage=(PFN_vkCreateImage)GetProcAddress(vk,"vkCreateImage");
  rawDestroyImage=(PFN_vkDestroyImage)GetProcAddress(vk,"vkDestroyImage");
  rawBarrier=(PFN_vkCmdPipelineBarrier)GetProcAddress(vk,"vkCmdPipelineBarrier");
  rawBegin=(PFN_vkBeginCommandBuffer)GetProcAddress(vk,"vkBeginCommandBuffer");
  rawReset=(PFN_vkResetCommandBuffer)GetProcAddress(vk,"vkResetCommandBuffer");
  rawSubmit=(PFN_vkQueueSubmit)GetProcAddress(vk,"vkQueueSubmit");
  rawSubmit2=(PFN_vkQueueSubmit2)GetProcAddress(vk,"vkQueueSubmit2");
  rawExecute=(PFN_vkCmdExecuteCommands)GetProcAddress(vk,"vkCmdExecuteCommands");
  o_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  o_CreateInstance_export = (PFN_vkCreateInstance)GetProcAddress(vk,"vkCreateInstance");
  o_EnumerateExtensions=(PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(vk,"vkEnumerateDeviceExtensionProperties");
  o_GDPA_real = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
  o_GetSwapchainImagesKHR_export = (PFN_vkGetSwapchainImagesKHR)GetProcAddress(vk, "vkGetSwapchainImagesKHR");
  o_AcquireNextImageKHR_export   = (PFN_vkAcquireNextImageKHR)GetProcAddress(vk, "vkAcquireNextImageKHR");
  o_AcquireNextImage2KHR_export=(PFN_vkAcquireNextImage2KHR)GetProcAddress(vk,"vkAcquireNextImage2KHR");
  o_QueuePresentKHR_export       = (PFN_vkQueuePresentKHR)GetProcAddress(vk, "vkQueuePresentKHR");
  o_CreateSwapchainKHR_export=(PFN_vkCreateSwapchainKHR)GetProcAddress(vk,"vkCreateSwapchainKHR");
  if(!o_GIPA || !o_GDPA_real){RuntimeErrors::ReportError(RuntimeErrors::Code::HookInstall,"Vulkan dispatch exports missing");Log("InstallVkHooks: vkGet*ProcAddr export missing");return false;}
  LONG transaction=DetourTransactionBegin();
  if(transaction!=NO_ERROR){RuntimeErrors::ReportError(RuntimeErrors::Code::HookInstall,"Vulkan hook transaction could not begin",(int)transaction);return false;}
  LONG attachResult=DetourUpdateThread(GetCurrentThread());
  auto attach=[&](PVOID* target,PVOID replacement){if(attachResult==NO_ERROR)attachResult=DetourAttach(target,replacement);};
  if(IsDX12Bridge(Cfg().provider)){
    rawCreatePool=(PFN_vkCreateCommandPool)GetProcAddress(vk,"vkCreateCommandPool");
    rawDestroyPool=(PFN_vkDestroyCommandPool)GetProcAddress(vk,"vkDestroyCommandPool");
    rawResetPool=(PFN_vkResetCommandPool)GetProcAddress(vk,"vkResetCommandPool");
    rawAllocateCommands=(PFN_vkAllocateCommandBuffers)GetProcAddress(vk,"vkAllocateCommandBuffers");
    rawFreeCommands=(PFN_vkFreeCommandBuffers)GetProcAddress(vk,"vkFreeCommandBuffers");
    if(rawCreatePool)attach(&(PVOID&)rawCreatePool,(PVOID)CaptureCreatePool);
    if(rawDestroyPool)attach(&(PVOID&)rawDestroyPool,(PVOID)CaptureDestroyPool);
    if(rawResetPool)attach(&(PVOID&)rawResetPool,(PVOID)CaptureResetPool);
    if(rawAllocateCommands)attach(&(PVOID&)rawAllocateCommands,(PVOID)CaptureAllocateCommands);
    if(rawFreeCommands)attach(&(PVOID&)rawFreeCommands,(PVOID)CaptureFreeCommands);
    o_CreateWin32SurfaceKHR_export=(PFN_vkCreateWin32SurfaceKHR)GetProcAddress(vk,"vkCreateWin32SurfaceKHR");
    o_DestroySwapchainKHR_export=(PFN_vkDestroySwapchainKHR)GetProcAddress(vk,"vkDestroySwapchainKHR");
    if(o_CreateWin32SurfaceKHR_export)attach(&(PVOID&)o_CreateWin32SurfaceKHR_export,(PVOID)h_SurfaceExport);
    if(o_DestroySwapchainKHR_export)attach(&(PVOID&)o_DestroySwapchainKHR_export,(PVOID)h_DestroySwapchainExport);
  }
  if(rawCreateImage)attach(&(PVOID&)rawCreateImage,(PVOID)CaptureCreateImage);
  if(rawDestroyImage)attach(&(PVOID&)rawDestroyImage,(PVOID)CaptureDestroyImage);
  if(rawBarrier)attach(&(PVOID&)rawBarrier,(PVOID)CaptureBarrier);
  if(rawBegin)attach(&(PVOID&)rawBegin,(PVOID)CaptureBegin);
  if(rawReset)attach(&(PVOID&)rawReset,(PVOID)CaptureReset);
  if(rawSubmit)attach(&(PVOID&)rawSubmit,(PVOID)CaptureSubmit);
  if(rawSubmit2)attach(&(PVOID&)rawSubmit2,(PVOID)CaptureSubmit2);
  if(rawExecute)attach(&(PVOID&)rawExecute,(PVOID)CaptureExecute);
  attach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  if(o_CreateInstance_export)attach(&(PVOID&)o_CreateInstance_export,(PVOID)h_CreateInstanceExport);
  if(o_EnumerateExtensions)attach(&(PVOID&)o_EnumerateExtensions,(PVOID)GameExtensions);
  attach(&(PVOID&)o_GDPA_real,(PVOID)h_GetDeviceProcAddrExport);
  if(o_GetSwapchainImagesKHR_export) attach(&(PVOID&)o_GetSwapchainImagesKHR_export,(PVOID)h_GetSwapchainImagesKHRExport);
  if(o_AcquireNextImageKHR_export)   attach(&(PVOID&)o_AcquireNextImageKHR_export,(PVOID)h_AcquireNextImageKHRExport);
  if(o_AcquireNextImage2KHR_export)attach(&(PVOID&)o_AcquireNextImage2KHR_export,(PVOID)h_AcquireNextImage2KHRExport);
  if(o_QueuePresentKHR_export)       attach(&(PVOID&)o_QueuePresentKHR_export,(PVOID)h_QueuePresentKHRExport);
  if(o_CreateSwapchainKHR_export)attach(&(PVOID&)o_CreateSwapchainKHR_export,(PVOID)h_CreateSwapchainKHRExport);
  if(attachResult!=NO_ERROR){DetourTransactionAbort();RuntimeErrors::ReportError(RuntimeErrors::Code::HookInstall,"Vulkan detour attach failed; transaction rolled back",(int)attachResult);Log("Vulkan hooks not committed: result=%ld",attachResult);return false;}
  LONG r = DetourTransactionCommit();
  if(r!=NO_ERROR)RuntimeErrors::ReportError(RuntimeErrors::Code::HookInstall,"Vulkan hook transaction failed; original dispatch retained",(int)r);
  Log("InstallVkHooks: exports hooked commit=%ld (GIPA=%p GDPA=%p getImages=%p acquire=%p present=%p)", r,
      (void*)o_GIPA,(void*)o_GDPA_real,(void*)o_GetSwapchainImagesKHR_export,(void*)o_AcquireNextImageKHR_export,(void*)o_QueuePresentKHR_export);
  return r==NO_ERROR;
}

void RemoveVkHooks(){
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  if(IsDX12Bridge(Cfg().provider)){if(o_CreateWin32SurfaceKHR_export)DetourDetach(&(PVOID&)o_CreateWin32SurfaceKHR_export,(PVOID)h_SurfaceExport);if(o_DestroySwapchainKHR_export)DetourDetach(&(PVOID&)o_DestroySwapchainKHR_export,(PVOID)h_DestroySwapchainExport);}
  if(IsDX12Bridge(Cfg().provider)){
    if(rawCreatePool)DetourDetach(&(PVOID&)rawCreatePool,(PVOID)CaptureCreatePool);
    if(rawDestroyPool)DetourDetach(&(PVOID&)rawDestroyPool,(PVOID)CaptureDestroyPool);
    if(rawResetPool)DetourDetach(&(PVOID&)rawResetPool,(PVOID)CaptureResetPool);
    if(rawAllocateCommands)DetourDetach(&(PVOID&)rawAllocateCommands,(PVOID)CaptureAllocateCommands);
    if(rawFreeCommands)DetourDetach(&(PVOID&)rawFreeCommands,(PVOID)CaptureFreeCommands);
  }
  if(rawCreateImage)DetourDetach(&(PVOID&)rawCreateImage,(PVOID)CaptureCreateImage);
  if(rawDestroyImage)DetourDetach(&(PVOID&)rawDestroyImage,(PVOID)CaptureDestroyImage);
  if(rawBarrier)DetourDetach(&(PVOID&)rawBarrier,(PVOID)CaptureBarrier);
  if(rawBegin)DetourDetach(&(PVOID&)rawBegin,(PVOID)CaptureBegin);
  if(rawReset)DetourDetach(&(PVOID&)rawReset,(PVOID)CaptureReset);
  if(rawSubmit)DetourDetach(&(PVOID&)rawSubmit,(PVOID)CaptureSubmit);
  if(rawSubmit2)DetourDetach(&(PVOID&)rawSubmit2,(PVOID)CaptureSubmit2);
  if(rawExecute)DetourDetach(&(PVOID&)rawExecute,(PVOID)CaptureExecute);
  DetourDetach(&(PVOID&)o_GIPA,(PVOID)h_GetInstanceProcAddr);
  if(o_CreateInstance_export)DetourDetach(&(PVOID&)o_CreateInstance_export,(PVOID)h_CreateInstanceExport);
  if(o_EnumerateExtensions)DetourDetach(&(PVOID&)o_EnumerateExtensions,(PVOID)GameExtensions);
  DetourDetach(&(PVOID&)o_GDPA_real,(PVOID)h_GetDeviceProcAddrExport);
  if(o_GetSwapchainImagesKHR_export) DetourDetach(&(PVOID&)o_GetSwapchainImagesKHR_export,(PVOID)h_GetSwapchainImagesKHRExport);
  if(o_AcquireNextImageKHR_export)   DetourDetach(&(PVOID&)o_AcquireNextImageKHR_export,(PVOID)h_AcquireNextImageKHRExport);
  if(o_AcquireNextImage2KHR_export)DetourDetach(&(PVOID&)o_AcquireNextImage2KHR_export,(PVOID)h_AcquireNextImage2KHRExport);
  if(o_QueuePresentKHR_export)       DetourDetach(&(PVOID&)o_QueuePresentKHR_export,(PVOID)h_QueuePresentKHRExport);
  if(o_CreateSwapchainKHR_export)DetourDetach(&(PVOID&)o_CreateSwapchainKHR_export,(PVOID)h_CreateSwapchainKHRExport);
  if(o_CreateDevice_export)          DetourDetach(&(PVOID&)o_CreateDevice_export,(PVOID)h_CreateDeviceExport);
  DetourTransactionCommit();
}
}

