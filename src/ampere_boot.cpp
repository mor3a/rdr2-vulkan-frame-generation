#include <windows.h>
#include <dxgi1_2.h>
#include <cwchar>
#include <cstring>
#include "log.h"
#include "ampere_boot.h"
#include "turing_unlock.h"
#include "core_diagnostics.h"
#include "config.h"
#include "runtime_errors.h"

namespace fgvk {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
wchar_t g_gpuName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};
GpuFamily g_family = GpuFamily::Unknown;
enum class Unlock { NotAttempted, Loaded, TuringLoaded, AdaLoaded, Failed, TuringFailed, AdaFailedNative, Native, Unsupported, UnsupportedDevice };
Unlock g_unlock = Unlock::NotAttempted;

GpuFamily Classify(const wchar_t* name) {
  if (!name) return GpuFamily::Unknown;
  if (wcsstr(name,L"RTX 50") || wcsstr(name,L"RTX 5050") || wcsstr(name,L"RTX 5060") || wcsstr(name,L"RTX 5070") || wcsstr(name,L"RTX 5080") || wcsstr(name,L"RTX 5090")) return GpuFamily::RTX50;
  if (wcsstr(name,L"RTX 40")) return GpuFamily::RTX40;
  if (wcsstr(name,L"RTX 30")) return GpuFamily::RTX30;
  if (wcsstr(name,L"RTX 20")) return GpuFamily::RTX20;
  return GpuFamily::Unknown;
}
void DetectDxgi() {
  AcquireSRWLockExclusive(&g_lock);
  if (g_gpuName[0]) { ReleaseSRWLockExclusive(&g_lock); return; }
  IDXGIFactory1* factory{};
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))) {
    for (UINT i=0;;++i) {
      IDXGIAdapter1* adapter{};
      const HRESULT enumerated=factory->EnumAdapters1(i,&adapter);
      if (enumerated==DXGI_ERROR_NOT_FOUND) break;
      if (FAILED(enumerated) || !adapter) { if(adapter) adapter->Release(); break; }
      DXGI_ADAPTER_DESC1 desc{};
      if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId==0x10de) {
        wcscpy_s(g_gpuName,desc.Description); g_family=Classify(g_gpuName); adapter->Release(); break;
      }
      adapter->Release();
    }
    factory->Release();
  }
  ReleaseSRWLockExclusive(&g_lock);
}
void IniPath(wchar_t* out, size_t count) {
  HMODULE self{};
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&IniPath),&self);
  GetModuleFileNameW(self,out,(DWORD)count);
  wchar_t* slash=wcsrchr(out,L'\\');
  if(!slash){out[0]=0;return;}
  *(slash+1)=0;
  wcscat_s(out,count,L"RDR2VulkanFG.ini");
}
}

void SetGpuInfo(const VkPhysicalDeviceProperties& properties) {
  AcquireSRWLockExclusive(&g_lock);
  wchar_t converted[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};
  MultiByteToWideChar(CP_UTF8,0,properties.deviceName,-1,converted,(int)(sizeof(converted)/sizeof(converted[0])));
  if (converted[0]) { wcscpy_s(g_gpuName,converted); g_family=Classify(g_gpuName); }
  ReleaseSRWLockExclusive(&g_lock);
  Log("GPU detected from Vulkan: %ls (family=%u)",g_gpuName,(unsigned)g_family);
}
const char* GPUName() {
  DetectDxgi();
  static thread_local char utf8[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE*4]{};
  AcquireSRWLockShared(&g_lock);
  WideCharToMultiByte(CP_UTF8,0,g_gpuName,-1,utf8,(int)sizeof(utf8),nullptr,nullptr);
  ReleaseSRWLockShared(&g_lock);
  return utf8[0]?utf8:"GPU not detected";
}
GpuFamily GPUFamily() { DetectDxgi(); AcquireSRWLockShared(&g_lock); auto f=g_family; ReleaseSRWLockShared(&g_lock); return f; }
GpuFgSupport GPUFrameGenerationSupport() {
  switch (GPUFamily()) {
    case GpuFamily::RTX20: return GpuFgSupport::ExperimentalTuring;
    case GpuFamily::RTX30: return GpuFgSupport::AmpereUnlock;
    case GpuFamily::RTX40: case GpuFamily::RTX50: return GpuFgSupport::Native;
    default: return GpuFgSupport::Unknown;
  }
}
bool ExperimentalTuringEnabled() {
  return Cfg().experimentalRTX20;
}
const char* GPUUnlockStatus() {
  AcquireSRWLockShared(&g_lock); Unlock state=g_unlock; ReleaseSRWLockShared(&g_lock);
  switch (state) {
    case Unlock::Loaded: return "RTX 30 unlock loaded";
    case Unlock::TuringLoaded: return "RTX 20 unlock 0.4.0 loaded; D3D12 bridge; provider validation pending";
    case Unlock::AdaLoaded: return "RTX 40 MFG core loaded; multiplier capacity is provider-reported";
    case Unlock::Failed: return "RTX 30 unlock failed";
    case Unlock::TuringFailed: return "experimental RTX 20 unlock failed";
    case Unlock::AdaFailedNative: return "RTX 40 MFG core unavailable; native frame generation fallback";
    case Unlock::Native: return "RTX 50 native frame/multi-frame generation; provider-reported capacity";
    case Unlock::Unsupported: return "RTX 20 native DLSS-G unsupported";
    case Unlock::UnsupportedDevice: return "GPU family not recognized; unlock disabled for safety";
    default: return GPUFamily()==GpuFamily::RTX20 && !ExperimentalTuringEnabled()
        ? "RTX 20 experimental unlock disabled; set EnableExperimentalRTX20=1 in RDR2VulkanFG.ini"
        : "unlock not attempted";
  }
}

bool LoadAmpereUnlock(){
  static bool attempted=false,ready=false;
  if(attempted)return ready || GPUFamily()==GpuFamily::RTX40;attempted=true;
  const GpuFamily family=GPUFamily();
  const bool turing=family==GpuFamily::RTX20;
  const bool ada=family==GpuFamily::RTX40;
  if(turing && !ExperimentalTuringEnabled()){
    AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::Unsupported;ReleaseSRWLockExclusive(&g_lock);
    Log("RTX 20/Turing detected; RTX 20 D3D12 unlock route disabled (set EnableExperimentalRTX20=1 to opt in)");return false;
  }
  if(family==GpuFamily::RTX50){ready=true;AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::Native;ReleaseSRWLockExclusive(&g_lock);Log("RTX 50 detected; native DLSS-G/MFG path; multiplier capacity will be queried from the provider");return true;}
  if(family!=GpuFamily::RTX30 && !turing && !ada){AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::UnsupportedDevice;ReleaseSRWLockExclusive(&g_lock);Log("GPU family not recognized as RTX 30/40/50 or opted-in RTX 20; refusing private unlock");return false;}
  if(ada)Log("RTX 40 detected; loading bundled Ada-capable MFG core before Streamline init (native FG alone is limited to 2x)");
  const auto failedState=ada?Unlock::AdaFailedNative:turing?Unlock::TuringFailed:Unlock::Failed;
  if(GetModuleHandleW(L"RTX40MFGCore.dll")){
    RuntimeErrors::ReportError(RuntimeErrors::Code::UnlockInit,"An MFG core was already loaded; refusing conflicting unlock stacks");Log("An MFG core is already loaded; refusing two unlock stacks");AcquireSRWLockExclusive(&g_lock);g_unlock=failedState;ReleaseSRWLockExclusive(&g_lock);return ada;
  }
  HMODULE self{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&LoadAmpereUnlock),&self);
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(self,path,MAX_PATH);
  wchar_t* slash=wcsrchr(path,L'\\');if(!slash){AcquireSRWLockExclusive(&g_lock);g_unlock=failedState;ReleaseSRWLockExclusive(&g_lock);return ada;}*(slash+1)=0;
  wchar_t full[MAX_PATH]{};swprintf(full,MAX_PATH,L"%sRDR2VulkanFG\\Core\\RTX40MFGCore.dll",path);
  if(turing){
    wchar_t directory[MAX_PATH]{};swprintf(directory,MAX_PATH,L"%sRDR2VulkanFG\\Core\\RTX20",path);
    char error[320]{};
    ready=LoadTuringUnlock040(directory,error,sizeof(error),unsigned(ReadDLSSGPreset()));
    if(!ready){RuntimeErrors::ReportError(RuntimeErrors::Code::UnlockInit,error,(int)GetLastError());Log("%s",error);}
    else{RuntimeErrors::ClearError(RuntimeErrors::Code::UnlockInit);Log("RTX 20 unlock prepared: dlssg_for_sm86 0.4.0 (6f047bd), exact kernels, SR untouched; D3D12 bridge; provider creation remains separate");}
    AcquireSRWLockExclusive(&g_lock);g_unlock=ready?Unlock::TuringLoaded:Unlock::TuringFailed;ReleaseSRWLockExclusive(&g_lock);
    return ready;
  }
  HMODULE core=LoadLibraryExW(full,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
  if(!core){RuntimeErrors::ReportError(RuntimeErrors::Code::UnlockInit,"Bundled RTX40MFGCore.dll could not be loaded",(int)GetLastError());Log("Bundled MFG core could not load, Win32=%lu; RTX40 native fallback=%d",GetLastError(),(int)ada);AcquireSRWLockExclusive(&g_lock);g_unlock=failedState;ReleaseSRWLockExclusive(&g_lock);return ada;}
  using Loaded=BOOL(WINAPI*)();auto loaded=reinterpret_cast<Loaded>(GetProcAddress(core,"MfgUnlockCoreLoaded"));
  using WaitStartup=BOOL(WINAPI*)(DWORD);
  auto waitStartup=reinterpret_cast<WaitStartup>(GetProcAddress(core,"MfgUnlockWaitStartup"));
  const ULONGLONG started=GetTickCount64();
  ready=loaded && loaded() && waitStartup && waitStartup(15000);
  if(!ready)RuntimeErrors::ReportError(RuntimeErrors::Code::UnlockInit,"Bundled MFG startup exports were missing or preparation did not complete");
  else {RuntimeErrors::ClearError(RuntimeErrors::Code::UnlockInit);SetCoreIntervalTracing(Cfg().intervalLogging);}
  Log("Private unlock startup prepared=%d waitMs=%llu; provider validation remains separate",(int)ready,(unsigned long long)(GetTickCount64()-started));
  AcquireSRWLockExclusive(&g_lock);g_unlock=ready?(ada?Unlock::AdaLoaded:turing?Unlock::TuringLoaded:Unlock::Loaded):failedState;ReleaseSRWLockExclusive(&g_lock);
  Log("Bundled MFG core attached=%d; provider readiness not established; turing=%d ada=%d nativeFallback=%d",(int)ready,(int)turing,(int)ada,(int)(ada&&!ready));
  return ready || ada;
}
}
