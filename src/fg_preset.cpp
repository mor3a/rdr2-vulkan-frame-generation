#include "fg_preset.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <tlhelp32.h>
namespace fgvk {
namespace {
// NVAPI public NGX GetOverrideState V1 ABI, release 570+. Read-only query.
// https://github.com/NVIDIA/nvapi (nvapi.h / nvapi_interface.h).
// The original V1 has reserved[3]; newer headers name the first slot FG mode.
struct OverrideStateV1 {
 uint32_t version,pid;uint64_t sr,rr,fg;float scaling;
 uint32_t performance,presetSR,frameCount,presetFG,reserved[3];
};
static_assert(sizeof(OverrideStateV1)==64 && offsetof(OverrideStateV1,presetFG)==48,"NVAPI ABI mismatch");
using QueryInterface=void* (__cdecl*)(uint32_t);
using Initialize=int (__cdecl*)();
using GetState=int (__cdecl*)(OverrideStateV1*);
QueryInterface queryApi{};
#pragma pack(push,4)
union DrsValue { uint32_t value; unsigned char bytes[4100]; uint64_t alignment; };
struct DrsSetting { uint32_t version; unsigned short name[2048]; uint32_t id,type,location,predefined,valid; DrsValue defaults,current; };
struct DrsApplication { uint32_t version,predefined; unsigned short name[2048],friendly[2048],launcher[2048]; };
#pragma pack(pop)
void ReadProfileAndVersion(FGPresetSnapshot& value){
 if(queryApi){
  auto create=(int(__cdecl*)(void**))queryApi(0x0694d52e);
  auto load=(int(__cdecl*)(void*))queryApi(0x375dbd6b);
  auto find=(int(__cdecl*)(void*,const wchar_t*,void**,DrsApplication*))queryApi(0xeee566b2);
  auto get=(int(__cdecl*)(void*,void*,uint32_t,DrsSetting*))queryApi(0x73bf8338);
  auto destroy=(int(__cdecl*)(void*))queryApi(0xdad9cff8);
  if(create && load && find && get && destroy){
   void* session{};value.profileResult=create(&session);
   if(value.profileResult==0){
    value.profileResult=load(session);void* profile{};DrsApplication app{};app.version=sizeof(app)|(1u<<16);
    if(value.profileResult==0)value.profileResult=find(session,L"RDR2.exe",&profile,&app);
    if(value.profileResult==0){DrsSetting setting{};setting.version=sizeof(setting)|(1u<<16);value.profileResult=get(session,profile,0x10e41df1,&setting);if(value.profileResult==0 && setting.type==0)value.profilePreset=setting.current.value;}
    destroy(session);
   }
  }
 }
 auto core=GetModuleHandleW(L"RTX40MFGCore.dll");
 if(core){using Read=BOOL(WINAPI*)(uint32_t*,uint32_t*,uint32_t*,uint64_t*);auto read=(Read)GetProcAddress(core,"MfgUnlockPresetSnapshot");if(read)value.providerRead=read(&value.providerPreset,&value.presetRequested,&value.presetError,&value.presetReads)!=FALSE;}
 // NGX OTA providers can be loaded under a .bin name. Identify by exports.
 HMODULE module{};
 HANDLE modules=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
 if(modules!=INVALID_HANDLE_VALUE){
  MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);
  if(Module32FirstW(modules,&entry))do{
   if(GetProcAddress(entry.hModule,"NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") &&
      GetProcAddress(entry.hModule,"NVSDK_NGX_VULKAN_CreateFeature") &&
      !GetProcAddress(entry.hModule,"NVSDK_NGX_DirectSR_Create")){module=entry.hModule;break;}
  }while(Module32NextW(modules,&entry));
  CloseHandle(modules);
 }

 if(module){wchar_t path[MAX_PATH]{};GetModuleFileNameW(module,path,MAX_PATH);DWORD ignored{};DWORD length=GetFileVersionInfoSizeW(path,&ignored);
  if(length && length<1024*1024){auto bytes=new unsigned char[length];if(GetFileVersionInfoW(path,0,length,bytes)){VS_FIXEDFILEINFO* info{};UINT size{};if(VerQueryValueW(bytes,L"\\",(void**)&info,&size) && size>=sizeof(*info))snprintf(value.dllVersion,sizeof(value.dllVersion),"%u.%u.%u.%u",HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS));}delete[] bytes;}
 }
}
std::mutex snapshotMutex;FGPresetSnapshot snapshot;
GetState reader{};int setupResult=-1;std::once_flag initializeOnce;
void InitializeReader(){
 auto module=LoadLibraryExW(L"nvapi64.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!module)return;
 auto query=(QueryInterface)GetProcAddress(module,"nvapi_QueryInterface");if(!query)return;
 auto init=(Initialize)query(0x0150e828);if(!init)return;
 setupResult=init();if(setupResult)return;
 queryApi=query;
 reader=(GetState)query(0x3fd96fba);if(!reader)setupResult=-3;
}
DWORD WINAPI PresetWorker(void*){
 SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
 FGPresetSnapshot previous;bool first=true;
 for(;;){
  auto next=QueryFGPreset(GetCurrentProcessId());
  {std::lock_guard<std::mutex> lock(snapshotMutex);snapshot=next;}
  if(first || next.providerPreset!=previous.providerPreset || next.providerRead!=previous.providerRead || next.presetError!=previous.presetError || next.result!=previous.result || next.preset!=previous.preset || next.feedback!=previous.feedback || next.profilePreset!=previous.profilePreset || next.profileResult!=previous.profileResult || strcmp(next.dllVersion,previous.dllVersion)!=0){
   Log("DLSS-G preset feedback: NVAPI=%d preset=%u mask=0x%llx reported=%u",next.result,next.preset,(unsigned long long)next.feedback,(unsigned)next.reported);
   Log("DLSS-G profile: result=%d requested=%u loadedDLL=%s",next.profileResult,next.profilePreset,next.dllVersion);
   Log("DLSS-G provider preset: valid=%d value=%u requested=%u reads=%llu hookError=%u",next.providerRead,next.providerPreset,next.presetRequested,(unsigned long long)next.presetReads,next.presetError);
   first=false;previous=next;
  }
  Sleep(2000);
 }
}
}
FGPresetSnapshot QueryFGPreset(uint32_t processId){
 std::call_once(initializeOnce,InitializeReader);FGPresetSnapshot value;ReadProfileAndVersion(value);
 if(!reader){value.result=setupResult;return value;}
 OverrideStateV1 state{};state.version=sizeof(state)|(1u<<16);state.pid=processId;
 value.result=reader(&state);
 if(value.result==0){
  value.preset=state.presetFG;value.feedback=state.fg;
  // Only expose a letter when the driver reports preset feedback, without an NGX override error.
  value.reported=(state.fg&(1ull<<5)) && !(state.fg&0x1f0000ull) && state.presetFG!=0;
 }
 return value;
}
FGPresetSnapshot GetFGPresetSnapshot(){std::lock_guard<std::mutex> lock(snapshotMutex);return snapshot;}
void StartFGPresetMonitor(){
 static std::atomic<bool> started{false};if(started.exchange(true))return;
 HMODULE self{};
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&PresetWorker,&self)){started=false;return;}
 HANDLE worker=CreateThread(nullptr,0,PresetWorker,nullptr,0,nullptr);
 if(worker)CloseHandle(worker);else{started=false;Log("DLSS-G preset monitor: thread creation failed=%lu",GetLastError());}
}
const char* FGPresetName(uint32_t value,char* buffer,unsigned capacity){
 if(value>=1 && value<=26)snprintf(buffer,capacity,"%c",'A'+value-1);
 else if(value==0xfffffe)snprintf(buffer,capacity,"Default");
 else if(value==0xffffff)snprintf(buffer,capacity,"Latest");
 else snprintf(buffer,capacity,"Unknown (%u)",value);
 return buffer;
}
}

