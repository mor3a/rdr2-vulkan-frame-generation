#include <windows.h>
#include <dxgi1_2.h>
#include <cwchar>
#include <cstring>
#include "log.h"
#include "ampere_boot.h"

namespace fgvk {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
wchar_t g_gpuName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE]{};
GpuFamily g_family = GpuFamily::Unknown;
enum class Unlock { NotAttempted, Loaded, TuringLoaded, Failed, TuringFailed, Native, Unsupported, UnsupportedDevice };
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
  wchar_t path[MAX_PATH]{}; IniPath(path,MAX_PATH);
  return path[0] && GetPrivateProfileIntW(L"fgvk",L"EnableExperimentalRTX20",0,path)!=0;
}
const char* GPUUnlockStatus() {
  AcquireSRWLockShared(&g_lock); Unlock state=g_unlock; ReleaseSRWLockShared(&g_lock);
  switch (state) {
    case Unlock::Loaded: return "RTX 30 unlock loaded";
    case Unlock::TuringLoaded: return "experimental RTX 20 unlock loaded (untested on RTX 20 hardware)";
    case Unlock::Failed: return "RTX 30 unlock failed";
    case Unlock::TuringFailed: return "experimental RTX 20 unlock failed";
    case Unlock::Native: return "native driver support; unlock not needed";
    case Unlock::Unsupported: return "RTX 20 native DLSS-G unsupported";
    case Unlock::UnsupportedDevice: return "GPU family not recognized; unlock disabled for safety";
    default: return GPUFamily()==GpuFamily::RTX20 && !ExperimentalTuringEnabled()
        ? "RTX 20 experimental unlock disabled; set EnableExperimentalRTX20=1 in RDR2VulkanFG.ini"
        : "unlock not attempted";
  }
}

bool LoadAmpereUnlock(){
  static bool attempted=false,ready=false;
  if(attempted)return ready;attempted=true;
  const GpuFamily family=GPUFamily();
  const bool turing=family==GpuFamily::RTX20;
  if(turing && !ExperimentalTuringEnabled()){
    AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::Unsupported;ReleaseSRWLockExclusive(&g_lock);
    Log("RTX 20/Turing detected; experimental v1.2.2 Turing route disabled (set EnableExperimentalRTX20=1 to opt in)");return false;
  }
  if(family==GpuFamily::RTX40 || family==GpuFamily::RTX50){AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::Native;ReleaseSRWLockExclusive(&g_lock);Log("RTX 40/50 detected; native DLSS-G path, private Ampere unlock skipped");return true;}
  if(family!=GpuFamily::RTX30 && !turing){AcquireSRWLockExclusive(&g_lock);g_unlock=Unlock::UnsupportedDevice;ReleaseSRWLockExclusive(&g_lock);Log("GPU family not recognized as RTX 30/40/50 or opted-in RTX 20; refusing private unlock");return false;}
  if(GetModuleHandleW(L"RTX40MFGCore.dll")){
    Log("An Ampere core is already loaded; refusing two unlock stacks");AcquireSRWLockExclusive(&g_lock);g_unlock=turing?Unlock::TuringFailed:Unlock::Failed;ReleaseSRWLockExclusive(&g_lock);return false;
  }
  HMODULE self{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCWSTR>(&LoadAmpereUnlock),&self);
  wchar_t path[MAX_PATH]{};GetModuleFileNameW(self,path,MAX_PATH);
  wchar_t* slash=wcsrchr(path,L'\\');if(!slash)return false;*(slash+1)=0;
  wchar_t full[MAX_PATH]{};swprintf(full,MAX_PATH,L"%sRDR2VulkanFG\\Ampere\\RTX40MFGCore.dll",path);
  HMODULE core=LoadLibraryExW(full,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
  if(!core){Log("Private Ampere core could not load, Win32=%lu",GetLastError());AcquireSRWLockExclusive(&g_lock);g_unlock=turing?Unlock::TuringFailed:Unlock::Failed;ReleaseSRWLockExclusive(&g_lock);return false;}
  using Loaded=BOOL(WINAPI*)();auto loaded=reinterpret_cast<Loaded>(GetProcAddress(core,"MfgUnlockCoreLoaded"));
  ready=loaded && loaded();AcquireSRWLockExclusive(&g_lock);g_unlock=ready?(turing?Unlock::TuringLoaded:Unlock::Loaded):(turing?Unlock::TuringFailed:Unlock::Failed);ReleaseSRWLockExclusive(&g_lock);
  Log("Private RTX 30/20 unlock core attached=%d; provider readiness not established; turing=%d",(int)ready,(int)turing);
  return ready;
}
}
