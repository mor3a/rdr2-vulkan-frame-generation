// Preserve the game's NGX initialization ABI, including legacy Init signatures.
// Assembly shims save all volatile registers and tail-call the exact original.
#include <windows.h>
#include <detours.h>
#include <cstring>
#include <cstdio>
#include <vulkan/vulkan.h>
#include <atomic>
#include "resource_state.h"
#include "log.h"
extern "C" {
void* rdr_original_init=nullptr;
void* rdr_original_ext=nullptr;
void* rdr_original_ext2=nullptr;
void rdr_capture_init();
void rdr_capture_ext();
void rdr_capture_ext2();
void rdr_record_appid(unsigned long long appid,void* caller,unsigned kind){
  HMODULE callerModule{};char callerPath[MAX_PATH]{};
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(caller),&callerModule);
  GetModuleFileNameA(callerModule,callerPath,MAX_PATH);
  const char* callerBase=strrchr(callerPath,'\\');callerBase=callerBase?callerBase+1:callerPath;
  fgvk::Log("NGX Init variant=%u caller=%s requested ApplicationId=0x%llX (%llu); FG disabled",kind,callerBase,appid,appid);
  // Nested driver/snippet initialization must not overwrite the game's identity.
  if(!callerModule || _stricmp(callerBase,"RDR2.exe")!=0)return;
  char exe[MAX_PATH]{};GetModuleFileNameA(nullptr,exe,MAX_PATH);
  char* slash=strrchr(exe,'\\');if(slash)*(slash+1)=0;
  char path[MAX_PATH]{};snprintf(path,sizeof(path),"%sRDR2-NGX-Capture.ini",exe);
  char value[32]{};snprintf(value,sizeof(value),"0x%llX",appid);
  WritePrivateProfileStringA("Observed","RequestedApplicationId",value,path);
}
}
namespace fgvk {
using GetProc=FARPROC (WINAPI*)(HMODULE,LPCSTR);
static GetProc originalGetProc=nullptr;
using GetVoid=int (__cdecl*)(void*,const char*,void**);
using GetFloat=int (__cdecl*)(void*,const char*,float*);
using Eval=int (__cdecl*)(VkCommandBuffer,const void*,const void*,void*);
static Eval originalEvaluate=nullptr;
static GetVoid getVoid=nullptr;
static GetFloat getFloat=nullptr;
static PFN_vkGetInstanceProcAddr captureGipa=nullptr;
static PFN_vkGetDeviceProcAddr captureGdpa=nullptr;
static PFN_vkVoidFunction VKAPI_CALL CaptureGdpa(VkDevice device,const char* name){
  auto original=captureGdpa(device,name);if(!name || !original)return original;
  if(strcmp(name,"vkGetDeviceProcAddr")==0)return (PFN_vkVoidFunction)&CaptureGdpa;
  return WrapResourceFn(name,original);
}
static PFN_vkVoidFunction VKAPI_CALL CaptureGipa(VkInstance instance,const char* name){
  auto original=captureGipa(instance,name);if(!name || !original)return original;
  if(strcmp(name,"vkGetInstanceProcAddr")==0)return (PFN_vkVoidFunction)&CaptureGipa;
  if(strcmp(name,"vkGetDeviceProcAddr")==0){captureGdpa=(PFN_vkGetDeviceProcAddr)original;return (PFN_vkVoidFunction)&CaptureGdpa;}
  return WrapResourceFn(name,original);
}
struct CaptureImageView {VkImageView view;VkImage image;VkImageSubresourceRange range;VkFormat format;unsigned width,height;};
struct CaptureResource {union {CaptureImageView image;struct {VkBuffer buffer;unsigned size;} buffer;} resource;int type;bool writable;};
static int __cdecl CaptureEvaluate(VkCommandBuffer cmd,const void* handle,const void* params,void* callback){
  static std::atomic<bool> recorded{false};
  if(params && getVoid && !recorded.load()){
    void* depth=nullptr;void* motion=nullptr;void* bb=nullptr;
    auto p=const_cast<void*>(params);
    bool sr=getVoid(p,"DLSSG.Backbuffer",&bb)!=1 || !bb;
    if(sr && getVoid(p,"Depth",&depth)==1 && depth && getVoid(p,"MotionVectors",&motion)==1 && motion){
      auto* d=static_cast<CaptureResource*>(depth);auto* m=static_cast<CaptureResource*>(motion);
      if(d->type==0 && m->type==0 && d->resource.image.width && m->resource.image.width && !recorded.exchange(true)){
        float jx=0,jy=0,sx=0,sy=0;bool scalars=getFloat && getFloat(p,"Jitter.Offset.X",&jx)==1 &&
          getFloat(p,"Jitter.Offset.Y",&jy)==1 && getFloat(p,"MV.Scale.X",&sx)==1 && getFloat(p,"MV.Scale.Y",&sy)==1;
        Log("Native DLSS inputs: depth=%ux%u format=%u motion=%ux%u format=%u jitter=(%.4f,%.4f) mvScale=(%.4f,%.4f) scalarsAvailable=%d",
          d->resource.image.width,d->resource.image.height,(unsigned)d->resource.image.format,
          m->resource.image.width,m->resource.image.height,(unsigned)m->resource.image.format,jx,jy,sx,sy,(int)scalars);
        ImageState ds{},ms{};bool dk=FindImageState(cmd,d->resource.image.image,ds),mk=FindImageState(cmd,m->resource.image.image,ms);
        Log("Native input tracking: depthKnown=%d layout=%u usage=0x%x motionKnown=%d layout=%u usage=0x%x; mip/layers depth=%u/%u motion=%u/%u",
          (int)dk,(unsigned)ds.layout,ds.usage,(int)mk,(unsigned)ms.layout,ms.usage,
          d->resource.image.range.levelCount,d->resource.image.range.layerCount,m->resource.image.range.levelCount,m->resource.image.range.layerCount);
      }
    }
  }
  return originalEvaluate(cmd,handle,params,callback);
}
static FARPROC WINAPI CaptureGetProc(HMODULE module,LPCSTR name){
  FARPROC original=originalGetProc(module,name);
  if(!original || reinterpret_cast<uintptr_t>(name)<65536)return original;
  if(strcmp(name,"vkGetInstanceProcAddr")==0){captureGipa=(PFN_vkGetInstanceProcAddr)original;Log("Vulkan diagnostic dispatch capture armed");return (FARPROC)&CaptureGipa;}
  if(strcmp(name,"vkGetDeviceProcAddr")==0){captureGdpa=(PFN_vkGetDeviceProcAddr)original;return (FARPROC)&CaptureGdpa;}
  if(strcmp(name,"NVSDK_NGX_VULKAN_EvaluateFeature")==0){
    if(originalEvaluate && originalEvaluate!=(Eval)original)return original;
    auto gv=(GetVoid)originalGetProc(module,"NVSDK_NGX_Parameter_GetVoidPointer");
    auto gf=(GetFloat)originalGetProc(module,"NVSDK_NGX_Parameter_GetF");
    if(!gv){HMODULE core=GetModuleHandleA("nvngx.dll");if(core){gv=(GetVoid)originalGetProc(core,"NVSDK_NGX_Parameter_GetVoidPointer");gf=(GetFloat)originalGetProc(core,"NVSDK_NGX_Parameter_GetF");}}
    if(!gv)return original;
    originalEvaluate=(Eval)original;getVoid=gv;getFloat=gf;return (FARPROC)&CaptureEvaluate;
  }
  void** target=nullptr;void* wrapper=nullptr;
  if(strcmp(name,"NVSDK_NGX_VULKAN_Init")==0){target=&rdr_original_init;wrapper=(void*)&rdr_capture_init;}
  else if(strcmp(name,"NVSDK_NGX_VULKAN_Init_Ext")==0){target=&rdr_original_ext;wrapper=(void*)&rdr_capture_ext;}
  else if(strcmp(name,"NVSDK_NGX_VULKAN_Init_Ext2")==0){target=&rdr_original_ext2;wrapper=(void*)&rdr_capture_ext2;}
  else if(strcmp(name,"NVSDK_NGX_VULKAN_Init_with_ProjectID")==0){
    Log("NGX ProjectID initialization requested; numeric ApplicationId capture unavailable");return original;
  }else return original;
  // Refuse to share one shim across different NGX modules with different targets.
  auto previous=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(target),(void*)original,nullptr);
  if(previous && previous!=(void*)original){
    static volatile LONG reported=0;if(InterlockedCompareExchange(&reported,1,0)==0)Log("NGX capture ignored nested initialization from a different provider");return original;
  }
  return reinterpret_cast<FARPROC>(wrapper);
}
bool InstallNgxCapture(){
  originalGetProc=&GetProcAddress;
  LONG begin=DetourTransactionBegin();if(begin!=NO_ERROR)return false;
  DetourUpdateThread(GetCurrentThread());
  LONG attach=DetourAttach(reinterpret_cast<void**>(&originalGetProc),(void*)&CaptureGetProc);
  if(attach!=NO_ERROR){DetourTransactionAbort();return false;}
  LONG commit=DetourTransactionCommit();Log("NGX diagnostic capture hook commit=%ld",commit);
  return commit==NO_ERROR;
}
}
