// Experimental DLSS-G input pipeline: capture the game's DLSS-SR EvaluateFeature for depth/mvec/jitter, tag
// preserve Depth/MotionVectors and leave the early SR HUD-less color off by default, push
// sl::Constants (synthetic perspective, identity reprojection - also his recipe), and run the
// PCL/Reflex marker ladder across the eval thread and the present thread on ONE frame token per
// frame (see the frame-token section below for why, and how the two threads hand it over).
// Full Vulkan resource descriptions are mandatory (an undefined format aborts inside SL).
#include "inputs.h"
#include "dlss_activity.h"
#include "frame_association.h"
#include "frame_history.h"
#include "input_capture.h"
#include "render_reprojection.h"
#include "mvecscale.h"
#include "log.h"
#include "slboot.h"
#include "vkhooks.h"
#include "config.h"
#include "gpu_guard.h"
#include "xess_provider.h"
#include <windows.h>
#include <detours.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "input_contract.h"
#include "resource_state.h"
#include "rdr_camera.h"
#include "camera_frame.h"
#include "neutral_camera.h"
#include "runtime_errors.h"
#include "native_restart.h"
#include "sr_transition.h"
#include "native_hud.h"
#include <cstdio>

// ---- minimal NGX surface (no vendored NGX SDK; layouts match the driver ABI) ----------
typedef int NVSDK_NGX_Result;
static const NVSDK_NGX_Result NGX_Success = 0x1;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;
typedef void (*PFN_NVSDK_NGX_ProgressCallback_C)(float, bool*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetVoidPointer)(NVSDK_NGX_Parameter*, const char*, void**);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetF)(NVSDK_NGX_Parameter*, const char*, float*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetUI)(NVSDK_NGX_Parameter*, const char*, unsigned int*);
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxGetI)(NVSDK_NGX_Parameter*, const char*, int*);
// NVSDK_NGX_DLSS_Feature_Flags (nvsdk_ngx_defs.h) - read from the game's own DLSS-SR create call
enum : int { kNgxFlagIsHDR=1<<0, kNgxFlagMVLowRes=1<<1, kNgxFlagMVJittered=1<<2, kNgxFlagDepthInverted=1<<3,
             kNgxFlagDoSharpening=1<<4, kNgxFlagAutoExposure=1<<5, kNgxFlagAlphaUpscaling=1<<6 };
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxEvaluateFeature)(VkCommandBuffer, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C);

struct NVSDK_NGX_ImageViewInfo_VK {
  VkImageView ImageView; VkImage Image; VkImageSubresourceRange SubresourceRange;
  VkFormat Format; unsigned int Width; unsigned int Height;
};
struct NVSDK_NGX_BufferInfo_VK { VkBuffer Buffer; unsigned int SizeInBytes; };
struct NVSDK_NGX_Resource_VK {
  union { NVSDK_NGX_ImageViewInfo_VK ImageViewInfo; NVSDK_NGX_BufferInfo_VK BufferInfo; } Resource;
  int Type; bool ReadWrite;
};

namespace fgvk {
static std::atomic<bool> inputErrors[RuntimeErrors::Capacity]{};
static void InputError(RuntimeErrors::Code code,const char* detail,int native=0){RuntimeErrors::ReportError(code,detail,native);if(!inputErrors[(unsigned)code].exchange(true))Log("%s %s native=%d",RuntimeErrors::CodeId(code),detail,native);}
static void InputRecovered(RuntimeErrors::Code code){RuntimeErrors::ClearError(code);if(inputErrors[(unsigned)code].exchange(false))Log("%s recovered",RuntimeErrors::CodeId(code));}

// ---- state ----------------------------------------------------------------------------
static PFN_NgxEvaluateFeature o_NgxEvaluate{};
typedef NVSDK_NGX_Result (__cdecl *PFN_NgxCreateFeature)(VkCommandBuffer, unsigned int /*featureId*/,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
static PFN_NgxCreateFeature o_NgxCreateFeature{};
static PFN_NgxGetVoidPointer p_GetVoidPointer{};
static PFN_NgxGetF p_GetF{};
static PFN_NgxGetUI p_GetUI{};
static PFN_NgxGetI p_GetI{};
// The driver exports Create/Evaluate but no C Parameter_Get entry points.
// MSVC ABI reverses this overloaded Get group: void/D3D12/D3D11/I/UI/D/F/ULL.
// Slots verified with a Windows-MSVC-targeted compiler against the SDK declaration.
template<class T>static NVSDK_NGX_Result VirtualGet(NVSDK_NGX_Parameter* p,const char* name,T* out,unsigned slot){
  if(!p||!out)return (int)0xbad00005;
  auto table=*reinterpret_cast<void***>(p);
  using Getter=NVSDK_NGX_Result(__cdecl*)(NVSDK_NGX_Parameter*,const char*,T*);
  return reinterpret_cast<Getter>(table[slot])(p,name,out);
}
static NVSDK_NGX_Result GetVirtualVoid(NVSDK_NGX_Parameter* p,const char* n,void** v){return VirtualGet(p,n,v,8);}
static NVSDK_NGX_Result GetVirtualF(NVSDK_NGX_Parameter* p,const char* n,float* v){return VirtualGet(p,n,v,14);}
static NVSDK_NGX_Result GetVirtualUI(NVSDK_NGX_Parameter* p,const char* n,unsigned int* v){return VirtualGet(p,n,v,12);}
static NVSDK_NGX_Result GetVirtualI(NVSDK_NGX_Parameter* p,const char* n,int* v){return VirtualGet(p,n,v,11);}
// MSVC overload slots from nvsdk_ngx_params.h, paired with the getters above.
static void SetVirtualI(NVSDK_NGX_Parameter* p,const char* n,int v){
  using Setter=void(__cdecl*)(NVSDK_NGX_Parameter*,const char*,int);
  reinterpret_cast<Setter>((*reinterpret_cast<void***>(p))[3])(p,n,v);
}
static void SetVirtualF(NVSDK_NGX_Parameter* p,const char* n,float v){
  using Setter=void(__cdecl*)(NVSDK_NGX_Parameter*,const char*,float);
  reinterpret_cast<Setter>((*reinterpret_cast<void***>(p))[6])(p,n,v);
}
using CreateFeature1=NVSDK_NGX_Result(__cdecl*)(VkDevice,VkCommandBuffer,unsigned int,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
static CreateFeature1 originalCreateFeature1{};
static std::mutex g_featureMutex;
struct SrFeature {int flags;uint64_t generation;};
static std::unordered_map<const NVSDK_NGX_Handle*,SrFeature> g_srFeatures;
static uint64_t g_srGeneration=0;
static SrTransition g_srTransition; // protected by g_srMutex
static std::atomic<int> g_srCreateFlags{-1};   // the game's DLSS-SR feature flags (-1 = not seen yet)
static bool g_hooked=false; static int g_probeDelay=0;
static bool g_logHookLive=false, g_logFirstInputs=false, g_logTagFail=false, g_logConstFail=false,
            g_logHudTag=false, g_logTokenShift=false, g_logSubmitOk=false;

struct FrameInputs {
  int featureFlags=-1;
  bool valid=false;
  VkImage depthImage{}, mvecImage{};
  VkImageView depthView{}, mvecView{};
  VkFormat depthFormat{}, mvecFormat{};
  VkImageAspectFlags depthAspect{},mvecAspect{};
  uint32_t depthW=0, depthH=0, mvecW=0, mvecH=0;
  uint32_t renderW=0,renderH=0,activeMvW=0,activeMvH=0,depthX=0,depthY=0,mvecX=0,mvecY=0;
  float jitterX=0, jitterY=0, mvScaleX=0, mvScaleY=0;
  unsigned int reset=0;
};
static std::mutex g_srMutex;
static inline double NowMs(){ static const double factor=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return 1000.0/double(f.QuadPart);}();LARGE_INTEGER c{};QueryPerformanceCounter(&c);return double(c.QuadPart)*factor; }
static double g_maxTagMs=0, g_maxConstMs=0, g_maxHudMs=0, g_lastEvalMs=0, g_maxGapMs=0, g_maxSleepMs=0; static uint32_t g_evalN=0;
struct PreparedFrame { uint32_t index=0; bool ready=false; };

// ---- frame token lifecycle ------------------------------------------------------------
// ONE token per frame, shared by the marker ladder, the tags and the constants. Verified in
// the SL 2.12 source: slGetNewFrameToken(nullptr) advances a global counter on EVERY call, and
// sl.reflex publishes the ePresentStart marker's index as latency.markerPresentFrame - the
// index DLSS-G uses at present time to look up that frame's tags+constants (sl.common's
// frame-based tag store has no fallback). The old "+1 shift" dates from calling the token
// function twice per frame; with one call it made every present consume the PREVIOUS frame's
// inputs (sl.log: "Unable to find 'common' constants for frame 1 ... using last set for frame 2").
//
// Evaluate and present can run on different workers. Select inputs through GPU
// submission dependencies, not CPU arrival order. NVIDIA owns token handles;
// explicit indices and use-time checks also cover handle reuse during loading.
static DlssActivity dlssActivity;
static std::atomic<bool> g_evalSeen{false};     // DLSS-SR filed tags+constants since the last present
static std::atomic<uint32_t> g_evalTid{0};
// Reflex sleep is REQUIRED by the Reflex checklist regardless of mode; it was dropped earlier on
// the (wrong) belief that BG3 drives Reflex natively - bg3.exe has no Reflex integration at all.
// fgvk.ini ReflexSleep=0 is the kill switch if the driver's pacing misbehaves.
#define kReflexSleep (Cfg().reflexSleep)

struct FrameSlot { sl::FrameToken* tok=nullptr; uint32_t index=0; bool submitted=false; bool simStarted=false; bool midMarkers=false; bool presenting=false; bool recording=false; sl::Constants constants{}; bool haveConstants=false; uint32_t taggedIndex=0,constantsIndex=0; };
static FrameHistory g_history;
static FrameHistory::Decision g_presentDecision{};
static uint32_t g_nextFrameIndex=1;
static FrameSlot g_loadingPresent{};
static uint32_t g_presentIndex=0;
static bool g_presentHasInputs=false;
static bool g_presentQueued=false;
static bool g_presentReady=false;
static FrameAssociation g_association;
static std::mutex g_associationMutex;
static std::mutex g_qMutex;                       // guards the ring below (eval thread vs present thread)
static const uint32_t kQCap = 4;
static FrameSlot g_q[kQCap]; static uint32_t g_qHead=0, g_qCount=0;   // head = next to present
static FrameSlot* QFront(){ return g_qCount ? &g_q[g_qHead] : nullptr; }
static FrameSlot* QBack(){ return g_qCount ? &g_q[(g_qHead+g_qCount-1)%kQCap] : nullptr; }
static FrameSlot* QFind(uint32_t index){ for(uint32_t i=0;i<g_qCount;i++){ FrameSlot& s=g_q[(g_qHead+i)%kQCap]; if(s.index==index) return &s; } return nullptr; }
static void QRemove(uint32_t index){
  CommitFrameCamera(index,false);CancelRenderReprojection(index);
  for(uint32_t i=0;i<g_qCount;++i)if(g_q[(g_qHead+i)%kQCap].index==index){
    for(uint32_t j=i;j+1<g_qCount;++j)g_q[(g_qHead+j)%kQCap]=g_q[(g_qHead+j+1)%kQCap];
    --g_qCount;return;
  }
}
static void QPop(){ if(g_qCount){CommitFrameCamera(g_q[g_qHead].index,false);CancelRenderReprojection(g_q[g_qHead].index); g_qHead=(g_qHead+1)%kQCap; g_qCount--; } }
// All token allocation and SDK calls using queued tokens hold g_qMutex.
static bool TokenMatches(const FrameSlot& s){
  if(s.tok && s.index && (uint32_t)*s.tok==s.index)return true;
  char detail[192];snprintf(detail,sizeof(detail),"slot=%u SDK=%u tags=%u constants=%u; stale frame token rejected",s.index,s.tok?(uint32_t)*s.tok:0,s.taggedIndex,s.constantsIndex);
  InputError(RuntimeErrors::Code::FrameIndex,detail);
  g_history.Invalidate(FrameHistory::IndexMismatch);
  return false;
}
static bool NewFrameSlot(FrameSlot& slot){
  auto& fns=GetSlFns();sl::FrameToken* token=nullptr;
  if(!fns.getNewFrameToken || !g_nextFrameIndex)return false;
  const uint32_t index=g_nextFrameIndex++;
  const auto result=fns.getNewFrameToken(token,&index);
  if(result!=sl::Result::eOk || !token){
    InputError(RuntimeErrors::Code::FrameIndex,"NVIDIA frame token allocation failed.",(int)result);
    g_history.Invalidate(FrameHistory::IndexMismatch);return false;
  }
  slot=FrameSlot{};slot.tok=token;slot.index=index;
  return TokenMatches(slot);
}
// New SL frame token (advances SL's counter) pushed as the newest slot; drops the oldest if the
// evals are outrunning the presents (would mean two DLSS-SR evaluates per presented frame).
static FrameSlot* QPushNew(){
  if(g_qCount>=3 && (QFront()->presenting || QFront()->recording))return nullptr;
  FrameSlot fresh{};if(!NewFrameSlot(fresh))return nullptr;
  if(g_qCount>=3){ static bool l=false; if(!l){ l=true; Log("frame token queue full - evals outrunning presents, dropping oldest frame %u", g_q[g_qHead].index); } QPop();g_history.Invalidate(FrameHistory::QueueDrop); }
  FrameSlot& s = g_q[(g_qHead+g_qCount)%kQCap]; s=fresh; g_qCount++;
  return &s;
}
static void SimStart(FrameSlot& s){
  if(s.simStarted || !TokenMatches(s)) return; s.simStarted=true;
  auto& fns = GetSlFns(); if(fns.pclSetMarker && s.tok) fns.pclSetMarker(sl::PCLMarker::eSimulationStart,*s.tok);
}
// SimulationEnd + RenderSubmitStart: at the DLSS-SR evaluate when there is one (mid render
// submit), otherwise just before present so the ladder stays complete and ordered.
static void MidMarkers(FrameSlot& s){
  if(s.midMarkers || !TokenMatches(s)) return; s.midMarkers=true;
  auto& fns = GetSlFns(); if(!fns.pclSetMarker || !s.tok) return;
  fns.pclSetMarker(sl::PCLMarker::eSimulationEnd,*s.tok);
  fns.pclSetMarker(sl::PCLMarker::eRenderSubmitStart,*s.tok);
}

// UIColorAndAlpha transparent image (session lifetime)
static VkImage g_uiImage{}; static VkDeviceMemory g_uiMem{}; static VkImageView g_uiView{};
static uint32_t g_uiW=0, g_uiH=0; static bool g_uiFailed=false;

// ---- module probe ---------------------------------------------------------------------
template <typename Fn> static bool forEachNgxModule(Fn&& fn){
  static const wchar_t* known[] = { L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlss.dll" };
  for (auto n : known){ HMODULE m=GetModuleHandleW(n); if(m && fn(m,n)) return true; }
  using PEnum = BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
  static PEnum pEnum = (PEnum)GetProcAddress(GetModuleHandleA("kernel32.dll"),"K32EnumProcessModules");
  if(!pEnum) return false;
  DWORD needed=0; if(!pEnum(GetCurrentProcess(),nullptr,0,&needed)) return false;
  HMODULE mods[1024]; DWORD cap = needed < sizeof(mods) ? needed : (DWORD)sizeof(mods);
  if(!pEnum(GetCurrentProcess(),mods,cap,&needed)) return false;
  DWORD count = (needed<cap?needed:cap)/sizeof(HMODULE);
  for(DWORD i=0;i<count;i++){ wchar_t p[MAX_PATH]{}; if(!GetModuleFileNameW(mods[i],p,MAX_PATH)) continue;
    if(fn(mods[i],p)) return true; }
  return false;
}

// ---- resource helpers (ported) --------------------------------------------------------
static void DescribeResource(sl::Resource& r, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage){
  r.width=w; r.height=h; r.nativeFormat=(uint32_t)fmt; r.mipLevels=1; r.arrayLayers=1; r.flags=0;
  r.usage=(uint32_t)usage;
}

static bool EnsureUIColorImage(VkCommandBuffer cmd, uint32_t w, uint32_t h){
  if(g_uiFailed) return false;
  if(g_uiImage && g_uiW==w && g_uiH==h) return true;
  if(!gDevice || !gPhysicalDevice) return false;
  if(g_uiImage){ g_uiImage=VK_NULL_HANDLE; g_uiView=VK_NULL_HANDLE; } // leak on resize (rare, safe)

  auto vkCreateImage_ = (PFN_vkCreateImage)DeviceFn("vkCreateImage");
  auto vkGetImageMemoryRequirements_ = (PFN_vkGetImageMemoryRequirements)DeviceFn("vkGetImageMemoryRequirements");
  auto vkAllocateMemory_ = (PFN_vkAllocateMemory)DeviceFn("vkAllocateMemory");
  auto vkBindImageMemory_ = (PFN_vkBindImageMemory)DeviceFn("vkBindImageMemory");
  auto vkCreateImageView_ = (PFN_vkCreateImageView)DeviceFn("vkCreateImageView");
  auto vkDestroyImage_ = (PFN_vkDestroyImage)DeviceFn("vkDestroyImage");
  auto vkCmdPipelineBarrier_ = (PFN_vkCmdPipelineBarrier)DeviceFn("vkCmdPipelineBarrier");
  auto vkCmdClearColorImage_ = (PFN_vkCmdClearColorImage)DeviceFn("vkCmdClearColorImage");
  auto vkGetPDMemProps_ = (PFN_vkGetPhysicalDeviceMemoryProperties)LoaderFn("vkGetPhysicalDeviceMemoryProperties");
  if(!vkCreateImage_||!vkGetImageMemoryRequirements_||!vkAllocateMemory_||!vkBindImageMemory_
     ||!vkCreateImageView_||!vkCmdPipelineBarrier_||!vkCmdClearColorImage_||!vkGetPDMemProps_){
    g_uiFailed=true; Log("UI image: vulkan fns unresolved - UI tag disabled"); return false; }

  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType=VK_IMAGE_TYPE_2D; ici.format=VK_FORMAT_R8G8B8A8_UNORM; ici.extent={w,h,1};
  ici.mipLevels=1; ici.arrayLayers=1; ici.samples=VK_SAMPLE_COUNT_1_BIT; ici.tiling=VK_IMAGE_TILING_OPTIMAL;
  ici.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage img{};
  if(vkCreateImage_(gDevice,&ici,nullptr,&img)!=VK_SUCCESS){ g_uiFailed=true; Log("UI image create failed"); return false; }
  VkMemoryRequirements mr{}; vkGetImageMemoryRequirements_(gDevice,img,&mr);
  VkPhysicalDeviceMemoryProperties mp{}; vkGetPDMemProps_(gPhysicalDevice,&mp);
  uint32_t idx=UINT32_MAX;
  for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    if((mr.memoryTypeBits&(1u<<i)) && (mp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){ idx=i; break; }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
  VkDeviceMemory mem{};
  if(idx==UINT32_MAX || vkAllocateMemory_(gDevice,&mai,nullptr,&mem)!=VK_SUCCESS
     || vkBindImageMemory_(gDevice,img,mem,0)!=VK_SUCCESS){
    vkDestroyImage_(gDevice,img,nullptr); g_uiFailed=true; Log("UI image mem alloc/bind failed"); return false; }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image=img; vci.viewType=VK_IMAGE_VIEW_TYPE_2D; vci.format=ici.format;
  vci.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageView view{};
  if(vkCreateImageView_(gDevice,&vci,nullptr,&view)!=VK_SUCCESS){ g_uiFailed=true; Log("UI image view failed"); return false; }

  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toDst.srcAccessMask=0; toDst.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  toDst.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; toDst.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; toDst.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
  toDst.image=img; toDst.subresourceRange=range;
  vkCmdPipelineBarrier_(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&toDst);
  VkClearColorValue clear{};  // fully transparent
  vkCmdClearColorImage_(cmd,img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
  VkImageMemoryBarrier toRead=toDst;
  toRead.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; toRead.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  toRead.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; toRead.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier_(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&toRead);
  g_uiImage=img; g_uiMem=mem; g_uiView=view; g_uiW=w; g_uiH=h;
  Log("UIColorAndAlpha transparent layer created %ux%u (4th required DLSS-G tag live)", w, h);
  return true;
}

// ---- constants (identity reprojection; synthetic perspective) -------------------------
static sl::float4x4 MakeIdentity(){
  sl::float4x4 m; m.setRow(0,{1,0,0,0}); m.setRow(1,{0,1,0,0}); m.setRow(2,{0,0,1,0}); m.setRow(3,{0,0,0,1});
  return m;
}
static sl::float4x4 MakePerspective(float fovRad, float aspect, float zn, float zf){
  // Row-major D3D-style perspective (viewToClip). Identity reprojection carries the temporal
  // load in phase A; this feeds SL's validation + OFA setup.
  float f = 1.0f / tanf(fovRad*0.5f);
  sl::float4x4 m{};
  m.setRow(0,{f/aspect,0,0,0});
  m.setRow(1,{0,f,0,0});
  m.setRow(2,{0,0,zf/(zf-zn),1});
  m.setRow(3,{0,0,-zn*zf/(zf-zn),0});
  return m;
}
static sl::float4x4 MakePerspectiveInv(float fovRad, float aspect, float zn, float zf){
  float f = 1.0f / tanf(fovRad*0.5f);
  float A = zf/(zf-zn), B = -zn*zf/(zf-zn);
  sl::float4x4 m{};
  m.setRow(0,{aspect/f,0,0,0});
  m.setRow(1,{0,1.0f/f,0,0});
  m.setRow(2,{0,0,0,1.0f/B});
  m.setRow(3,{0,0,1,-A/B});
  return m;
}

// ---- frame submission (ported SubmitFrameData) ----------------------------------------
static bool PrepareFrameData(VkCommandBuffer cmd,const FrameInputs& input,PreparedFrame& prepared){
  prepared=PreparedFrame{};
  if(Cfg().provider==FGProvider::DLSSG && (Rt().fgUserOff.load() || NativeRestartPending() || NativeRestartFailed())){InvalidateRdrCameraHistory();return false;}
  auto& fns = GetSlFns();
  if(Cfg().provider==FGProvider::DLSSG && (!fns.getNewFrameToken || !fns.setTagForFrame || !fns.setConstants)) return false;

  ImageState ds{}, msState{};
  const bool depthTracked=FindImageState(cmd,input.depthImage,ds,input.depthAspect);
  const bool mvTracked=FindImageState(cmd,input.mvecImage,msState,input.mvecAspect);
  if(!depthTracked || !mvTracked){
    char detail[256];{std::lock_guard<std::mutex> lock(resource_detail::mutex);auto c=resource_detail::commands.find(cmd);snprintf(detail,sizeof(detail),"cmd=%p depth=%p tracked=%d layout=%d mv=%p tracked=%d layout=%d images=%zu commands=%zu thisCmd=%zu",(void*)cmd,(void*)input.depthImage,depthTracked,(int)ds.layout,(void*)input.mvecImage,mvTracked,(int)msState.layout,resource_detail::images.size(),resource_detail::commands.size(),c==resource_detail::commands.end()?0:c->second.size());}InputError(RuntimeErrors::Code::InputState,detail);
    static unsigned blocked=0;
    if(++blocked==1 || blocked%300==0){std::lock_guard<std::mutex> lock(resource_detail::mutex);auto c=resource_detail::commands.find(cmd);Log("FG blocked: metadata images=%zu commands=%zu thisCmd=%zu depthCreated=%d mvCreated=%d",resource_detail::images.size(),resource_detail::commands.size(),c==resource_detail::commands.end()?0:c->second.size(),resource_detail::images.count(input.depthImage)!=0,resource_detail::images.count(input.mvecImage)!=0);}
    return false;
  }
  if(!ReadableLayout(ds.layout)||!ReadableLayout(msState.layout)){char detail[256];snprintf(detail,sizeof(detail),"cmd=%p depth=%p layout=%d mv=%p layout=%d; observed layout is not readable",(void*)cmd,(void*)input.depthImage,(int)ds.layout,(void*)input.mvecImage,(int)msState.layout);InputError(RuntimeErrors::Code::InputState,detail);return false;}
  InputRecovered(RuntimeErrors::Code::InputState);
  int srf=input.featureFlags;if(srf<0)return false;
  float aspect=(float)input.renderW/(float)input.renderH;
  sl::Constants c{};
  if(IsDX12Bridge(Cfg().provider)){if(!BuildNeutralCamera(c,aspect,(srf&kNgxFlagDepthInverted)!=0,input.reset!=0))return false;bool ok=XessPrepare(cmd,input.depthImage,input.mvecImage,input.renderW,input.renderH,input.activeMvW,input.activeMvH,input.jitterX,input.jitterY,input.mvScaleX,input.mvScaleY,ds,msState,c,srf,{int32_t(input.depthX),int32_t(input.depthY)},{int32_t(input.mvecX),int32_t(input.mvecY)});prepared.ready=ok;if(ok)g_evalSeen.store(true);return ok;}

  // Take the newest frame slot; if it already carries a submission we have run ahead of the
  // previous frame's present (pipelined engine) and this evaluate belongs to a NEW frame.
  uint32_t slotIndex=0;
  { std::lock_guard<std::mutex> lk(g_qMutex);
    FrameSlot* b = QBack();
    if(!b || b->submitted || b->recording || b->presenting){ b = QPushNew(); if(!b){CommitRdrCamera(false);return false;} SimStart(*b); }
    if(!TokenMatches(*b)){QRemove(b->index);return false;}
    MidMarkers(*b);   // SimulationEnd + RenderSubmitStart (we are inside the render submit)
    b->recording=true; slotIndex = b->index; prepared.index=slotIndex; }
  g_evalTid.store((uint32_t)GetCurrentThreadId());
  if(!g_logTokenShift){ g_logTokenShift=true;
    Log("FG frame token %u: tags+constants+present markers share one token per frame (no +1 shift) evalTid=%lu",
        slotIndex, (unsigned long)GetCurrentThreadId()); }
  // Camera source is independent of the presentation backend in 0.80.
  // Pose selection belongs to this input token; history resolves at Present.
  // Depth, dense motion vectors, jitter and their NGX flags remain unchanged.
  if(!PrepareFrameCamera(slotIndex,c,aspect,(srf&kNgxFlagDepthInverted)!=0,input.reset!=0))return false;

  sl::ViewportHandle vp{0};
  // Never guess another game's attachment layouts. Only command-local observed
  // barriers and tracked image creation metadata may supply the tag description.
  sl::Resource depthRes(sl::ResourceType::eTex2d,(void*)input.depthImage,(void*)nullptr,
      (void*)input.depthView,(uint32_t)ds.layout);
  sl::Resource mvecRes(sl::ResourceType::eTex2d,(void*)input.mvecImage,(void*)nullptr,
      (void*)input.mvecView,(uint32_t)msState.layout);
  DescribeResource(depthRes,input.depthW,input.depthH,input.depthFormat,
      ds.usage);
  DescribeResource(mvecRes,input.mvecW,input.mvecH,input.mvecFormat,
      msState.usage);
  sl::Extent depthExtent{input.depthX,input.depthY,input.renderW,input.renderH};
  sl::Extent mvecExtent{input.mvecX,input.mvecY,input.activeMvW,input.activeMvH};
  sl::ResourceTag tags[] = {
    sl::ResourceTag(&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depthExtent),
    sl::ResourceTag(&mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &mvecExtent),
  };
  double _t0=NowMs();
  auto tagRes=sl::Result::eErrorInvalidParameter;
  {std::lock_guard<std::mutex> lock(g_qMutex);
    if(FrameSlot* slot=QFind(slotIndex);slot && TokenMatches(*slot)){
      tagRes=fns.setTagForFrame(*slot->tok,vp,tags,2,reinterpret_cast<sl::CommandBuffer*>(cmd));
      if(tagRes==sl::Result::eOk)slot->taggedIndex=slotIndex;
    }
  }
  double _tag=NowMs()-_t0; if(_tag>g_maxTagMs) g_maxTagMs=_tag;
  if(tagRes!=sl::Result::eOk && !g_logTagFail){ g_logTagFail=true; Log("slSetTagForFrame(depth/mvec) failed: %d",(int)tagRes); }
  if(tagRes!=sl::Result::eOk)InputError(RuntimeErrors::Code::TagsFailed,"slSetTagForFrame(depth/motion vectors) rejected the frame resources.",(int)tagRes);else InputRecovered(RuntimeErrors::Code::TagsFailed);

  c.cameraPinholeOffset = sl::float2(0.f,0.f);
  c.jitterOffset = sl::float2(input.jitterX, input.jitterY);
  // Streamline normalises: it multiplies this by the mvec extent before the driver sees it, so
  // raw NGX pixel-space values (-1,-1) must be divided by the extent (see mvecscale.h).
  MvecScale ms = MvecScaleForStreamline(input.mvScaleX, input.mvScaleY, input.activeMvW, input.activeMvH, Cfg().mvecScaleNormalized);
  c.mvecScale = sl::float2(ms.x, ms.y);
  // Flags straight from the game's DLSS-SR create call when we saw it (PureDark's capture agrees:
  // DepthInverted=1); until then the captured recipe's values.
  c.depthInverted = (srf & kNgxFlagDepthInverted) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.motionVectorsJittered = (srf > 0 && (srf & kNgxFlagMVJittered)) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.minRelativeLinearDepthObjectSeparation = Rt().depthSeparation.load(std::memory_order_relaxed);
  if(input.depthFormat==VK_FORMAT_D32_SFLOAT_S8_UINT && input.mvecFormat==VK_FORMAT_R16G16_SFLOAT)InputCaptureRecord(cmd,slotIndex,input.depthImage,input.mvecImage,input.depthW,input.depthH,input.mvecW,input.mvecH,ds.layout,msState.layout,c);
  double _c0=NowMs();
  // No GPU reprojection readback on the DLSS-G path anymore: constants are
  // complete at eval time (identity reprojection), so the ~14-16ms readback
  // fence wait and the fit are both off the present path.
  if(Cfg().cameraSource==CameraSource::Derived || Cfg().cameraAudit){
    if(!RecordRenderReprojection(cmd,slotIndex,input.depthImage,input.depthFormat,input.mvecImage,input.mvecFormat,input.renderW,input.renderH,ds.layout,msState.layout,c,input.depthX,input.depthY,input.mvecX,input.mvecY))CancelRenderReprojection(slotIndex);
  }
  bool cameraQueued=false;
  // Publish constants once, on the present thread, after this exact input
  // command has completed. ScriptHook timing must not select the render pose.
  // Store constants only now: jitterOffset/mvecScale/flags are assigned
  // above. Storing right after ApplyRdrCamera shipped FLT_MAX sentinels for
  // those fields and DLSS-G rejected every frame (CommonConstantsInvalid).
  { std::lock_guard<std::mutex> lk(g_qMutex); if(FrameSlot* s=QFind(slotIndex);s && TokenMatches(*s)){s->constants=c;s->haveConstants=true;s->constantsIndex=slotIndex;cameraQueued=true;} }
  auto constRes=cameraQueued?sl::Result::eOk:sl::Result::eErrorInvalidParameter;
  double _cms=NowMs()-_c0; if(_cms>g_maxConstMs) g_maxConstMs=_cms;
  if(tagRes==sl::Result::eOk && constRes==sl::Result::eOk && !g_logSubmitOk){
    g_logSubmitOk=true; Log("FG input copies queued; constants pending matched present (depth %ux%u, mvec %ux%u, jitter %.4f/%.4f mvScale %.1f/%.1f)",
      input.depthW,input.depthH,input.mvecW,input.mvecH,input.jitterX,input.jitterY,input.mvScaleX,input.mvScaleY); }
  double _now=NowMs(); double _gap=_now-g_lastEvalMs; if(g_lastEvalMs>0 && _gap>g_maxGapMs) g_maxGapMs=_gap; g_lastEvalMs=_now;
  if(DebugLoggingEnabled() && (++g_evalN % 120)==0){ Log("TIMING: maxTag=%.1fms maxCameraSetup=%.1fms maxHud=%.1fms maxReflexSleep=%.1fms maxFrameGap=%.1fms (over 120 evals)",
      g_maxTagMs,g_maxConstMs,g_maxHudMs,g_maxSleepMs,g_maxGapMs); g_maxTagMs=g_maxConstMs=g_maxHudMs=g_maxSleepMs=g_maxGapMs=0; }
  prepared.ready=tagRes==sl::Result::eOk && constRes==sl::Result::eOk;
  return prepared.ready;
}
// Preparation only records GPU work. Publish a frame after the original SR
// call accepted it; a failed evaluation must never enter the present ledger.
static void CommitFrameData(VkCommandBuffer cmd,const PreparedFrame& prepared,bool srSucceeded){
  if(!prepared.index)return;
  bool accepted=false;
  {std::lock_guard<std::mutex> lock(g_qMutex);
    if(FrameSlot* slot=QFind(prepared.index)){
      accepted=prepared.ready && srSucceeded && TokenMatches(*slot) && slot->taggedIndex==slot->index && slot->constantsIndex==slot->index;
      if(accepted){std::lock_guard<std::mutex> associationLock(g_associationMutex);g_association.Record((uint64_t)(uintptr_t)cmd,prepared.index);}
      slot->recording=false;slot->submitted=accepted;
    }
    if(!accepted){QRemove(prepared.index);g_history.Invalidate(FrameHistory::Rejected);}
  }
  if(!accepted)CommitFrameCamera(prepared.index,false);
  if(accepted)g_evalSeen.store(true);
  else {CancelRenderReprojection(prepared.index);Log("FG input transaction rejected: token=%u prepared=%d SRsuccess=%d",prepared.index,prepared.ready,srSucceeded);}
}

// Optional tags (fgvk.ini TagHUDLess / TagUI). Default OFF = backbuffer only, PureDark parity:
// his captured recipe carries no UI buffer and only occasionally a HUD-less one. The DLSS-SR
// output is pre-post-processing (linear/HDR before tonemap); the guide requires HUD-less to be
// in the SAME color space as the backbuffer, so feeding it can produce halos/ghosting.
static void TagHUDLessColor(uint32_t index,VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat fmt,
                            uint32_t w, uint32_t h){
  if(NativeHudEnabled())return; // use the final-color snapshot, never early linear SR output
  auto& fns = GetSlFns();
  if(!fns.setTagForFrame || !index) return;
  if(!g_logHudTag){ g_logHudTag=true;
    Log("DLSS-SR output %ux%u fmt=%d; optional tags: HUDLess=%d UI=%d", w, h, (int)fmt, (int)Cfg().tagHudless, (int)Cfg().tagUI); }
  if(!Cfg().tagHudless && !Cfg().tagUI) return;
  sl::Resource colorRes(sl::ResourceType::eTex2d,(void*)image,(void*)nullptr,
      (void*)view,(uint32_t)VK_IMAGE_LAYOUT_GENERAL);   // NGX writes Output as compute storage
  DescribeResource(colorRes,w,h,fmt,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  sl::Extent extent{0,0,w,h};
  bool haveUI = Cfg().tagUI && EnsureUIColorImage(cmd,w,h);
  sl::Resource uiRes(sl::ResourceType::eTex2d,(void*)g_uiImage,(void*)g_uiMem,
      (void*)g_uiView,(uint32_t)VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  DescribeResource(uiRes,w,h,VK_FORMAT_R8G8B8A8_UNORM,
      VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  sl::ResourceTag tags[2]; uint32_t n=0;
  if(Cfg().tagHudless) tags[n++] = sl::ResourceTag(&colorRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent);
  if(haveUI)           tags[n++] = sl::ResourceTag(&uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent);
  if(!n) return;
  sl::ViewportHandle vp{0};
  double _h0=NowMs();
  auto res=sl::Result::eErrorInvalidParameter;
  {std::lock_guard<std::mutex> lock(g_qMutex);if(FrameSlot* slot=QFind(index);slot && TokenMatches(*slot))res=fns.setTagForFrame(*slot->tok,vp,tags,n,reinterpret_cast<sl::CommandBuffer*>(cmd));}
  double _hms=NowMs()-_h0; if(_hms>g_maxHudMs) g_maxHudMs=_hms;
  static bool l2=false; if(!l2){ l2=true;
    if(res==sl::Result::eOk) Log("optional tags submitted (%u) %ux%u", n, w, h);
    else Log("slSetTagForFrame(HUDLess/UI) failed: %d",(int)res); }
}

// ---- NGX inputs read (ported readNgxFrameInputs) --------------------------------------
static void ReadFrameInputs(const NVSDK_NGX_Parameter* params,FrameInputs& input,int flags){
  input=FrameInputs{};input.featureFlags=flags;
  if(!params||!p_GetVoidPointer){InputError(RuntimeErrors::Code::InputInvalid,"NGX parameter object or resource getter unavailable.");return;}
  auto* p = const_cast<NVSDK_NGX_Parameter*>(params);
  auto readRes=[&](const char* name, VkImage& img, VkImageView& view, VkFormat& fmt, uint32_t& w, uint32_t& h, VkImageAspectFlags& aspect)->bool{
    void* ptr=nullptr;
    if(p_GetVoidPointer(p,name,&ptr)!=NGX_Success || !ptr) return false;
    auto* r=reinterpret_cast<NVSDK_NGX_Resource_VK*>(ptr);
    if(r->Type!=0) return false;
    auto& iv=r->Resource.ImageViewInfo;
    static unsigned resourceLogs=0;
    if(resourceLogs<2){++resourceLogs;Log("NGX resource %s type=%d image=%p view=%p fmt=%d %ux%u range=%u/%u/%u/%u aspect=%u",name,r->Type,(void*)iv.Image,(void*)iv.ImageView,(int)iv.Format,iv.Width,iv.Height,iv.SubresourceRange.baseMipLevel,iv.SubresourceRange.levelCount,iv.SubresourceRange.baseArrayLayer,iv.SubresourceRange.layerCount,iv.SubresourceRange.aspectMask);}
    if(!iv.Image||!iv.ImageView||iv.Format==VK_FORMAT_UNDEFINED ||
       !ValidExtent(iv.Width,iv.Height) || iv.SubresourceRange.baseMipLevel!=0 ||
       iv.SubresourceRange.baseArrayLayer!=0 || iv.SubresourceRange.levelCount!=1 ||
       iv.SubresourceRange.layerCount!=1) return false;
    img=iv.Image; view=iv.ImageView; fmt=iv.Format; w=iv.Width; h=iv.Height; aspect=iv.SubresourceRange.aspectMask;
    // RDR2 uses D32_SFLOAT_S8_UINT with a depth+stencil view. NGX consumes
    // depth; stencil is valid view metadata, not a malformed resource.
    if(aspect==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))aspect=VK_IMAGE_ASPECT_DEPTH_BIT;
    return aspect==VK_IMAGE_ASPECT_COLOR_BIT || aspect==VK_IMAGE_ASPECT_DEPTH_BIT;
  };
  bool depthOk=readRes("Depth",input.depthImage,input.depthView,input.depthFormat,input.depthW,input.depthH,input.depthAspect);
  bool mvecOk=readRes("MotionVectors",input.mvecImage,input.mvecView,input.mvecFormat,input.mvecW,input.mvecH,input.mvecAspect);
  auto optionalUI=[&](const char* key,uint32_t fallback){unsigned value=0;return p_GetUI && p_GetUI(p,key,&value)==NGX_Success?value:fallback;};
  input.renderW=optionalUI("DLSS.Render.Subrect.Dimensions.Width",input.depthW);
  input.renderH=optionalUI("DLSS.Render.Subrect.Dimensions.Height",input.depthH);
  if(!input.renderW)input.renderW=input.depthW;if(!input.renderH)input.renderH=input.depthH;
  input.depthX=optionalUI("DLSS.Input.Depth.Subrect.Base.X",0);input.depthY=optionalUI("DLSS.Input.Depth.Subrect.Base.Y",0);
  input.mvecX=optionalUI("DLSS.Input.MV.Subrect.Base.X",0);input.mvecY=optionalUI("DLSS.Input.MV.Subrect.Base.Y",0);
  const bool lowRes=(input.featureFlags&kNgxFlagMVLowRes)!=0;
  input.activeMvW=lowRes?input.renderW:input.mvecW;input.activeMvH=lowRes?input.renderH:input.mvecH;
  auto fits=[](uint32_t x,uint32_t y,uint32_t w,uint32_t h,uint32_t bw,uint32_t bh){return w&&h&&x<=bw&&y<=bh&&w<=bw-x&&h<=bh-y;};
  const bool rectsOk=fits(input.depthX,input.depthY,input.renderW,input.renderH,input.depthW,input.depthH)&&fits(input.mvecX,input.mvecY,input.activeMvW,input.activeMvH,input.mvecW,input.mvecH);
  if(!rectsOk){InputError(RuntimeErrors::Code::InputInvalid,"NGX active render rectangle exceeds its depth/motion image");return;}
  static uint32_t priorW=0,priorH=0;
  if(priorW!=input.renderW||priorH!=input.renderH){priorW=input.renderW;priorH=input.renderH;Log("NGX active render rectangle: %ux%u depthOrigin=%u,%u motionOrigin=%u,%u motionExtent=%ux%u",priorW,priorH,input.depthX,input.depthY,input.mvecX,input.mvecY,input.activeMvW,input.activeMvH);}
  input.jitterX=input.jitterY=input.mvScaleX=input.mvScaleY=0; input.reset=0;
  bool scalarsOk = p_GetF &&
      p_GetF(p,"Jitter.Offset.X",&input.jitterX)==NGX_Success &&
      p_GetF(p,"Jitter.Offset.Y",&input.jitterY)==NGX_Success &&
      p_GetF(p,"MV.Scale.X",&input.mvScaleX)==NGX_Success &&
      p_GetF(p,"MV.Scale.Y",&input.mvScaleY)==NGX_Success;
  scalarsOk = scalarsOk && ValidScalars(input.jitterX,input.jitterY,input.mvScaleX,input.mvScaleY);
  if(p_GetUI){ p_GetUI(p,"Reset",&input.reset); }
  if(!depthOk||!mvecOk||!scalarsOk || input.featureFlags<0){char detail[256];snprintf(detail,sizeof(detail),"NGX rejected: depth=%d mvec=%d scalars=%d jitter=%f/%f scale=%f/%f flags=%d",depthOk,mvecOk,scalarsOk,input.jitterX,input.jitterY,input.mvScaleX,input.mvScaleY,input.featureFlags);InputError(RuntimeErrors::Code::InputInvalid,detail);return;}
  InputRecovered(RuntimeErrors::Code::InputInvalid);
  input.valid=true;
  if(!g_logFirstInputs){ g_logFirstInputs=true;
    Log("NGX inputs first valid: depth %ux%u fmt=%d, mvec %ux%u fmt=%d, jitter=(%.4f,%.4f) mvScale=(%.1f,%.1f)",
        input.depthW,input.depthH,(int)input.depthFormat,input.mvecW,input.mvecH,(int)input.mvecFormat,
        input.jitterX,input.jitterY,input.mvScaleX,input.mvScaleY); }
}

// ---- the evaluate detour --------------------------------------------------------------
static thread_local bool g_inEval=false;
static NVSDK_NGX_Result __cdecl h_NgxEvaluate(VkCommandBuffer cmd, const NVSDK_NGX_Handle* h,
    const NVSDK_NGX_Parameter* params, PFN_NVSDK_NGX_ProgressCallback_C cb){
  if(IsGpuBlocked() || g_inEval) return o_NgxEvaluate(cmd,h,params,cb);
  g_inEval=true;
  extern std::atomic<uint32_t> g_wdEvals; extern std::atomic<int> g_wdPos;
  g_wdEvals.fetch_add(1); g_wdPos.store(10);
  if(!g_logHookLive){ g_logHookLive=true; Log("NGX EvaluateFeature hook live"); }

  // Filter out sl.dlss_g's OWN evaluates (they carry DLSSG params; ours must only snoop
  // the game's DLSS-SR). Without this, generation re-enters the snoop with its own frames.
  bool isDlssg=false;
  if(params && p_GetVoidPointer){
    void* bb=nullptr;
    if(p_GetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(params),"DLSSG.Backbuffer",&bb)==NGX_Success && bb)
      isDlssg=true;
  }
  if(isDlssg){ g_inEval=false; return o_NgxEvaluate(cmd,h,params,cb); }
  int featureFlags=-1;uint64_t featureGeneration=0;
  {std::lock_guard<std::mutex> lock(g_featureMutex);auto feature=g_srFeatures.find(h);if(feature!=g_srFeatures.end()){featureFlags=feature->second.flags;featureGeneration=feature->second.generation;}}
  // The handle must come from a successful SR creation. Other NGX features
  // and failed creations cannot inherit whichever flags were read last.
  if(featureFlags<0){g_inEval=false;g_wdPos.store(0);return o_NgxEvaluate(cmd,h,params,cb);}
  std::lock_guard<std::mutex> srLock(g_srMutex);
  FrameInputs input{};PreparedFrame prepared{};
  if(params)ReadFrameInputs(params,input,featureFlags);
  const bool qualityTransition=Cfg().provider==FGProvider::DLSSG && input.valid && g_srTransition.Changed(featureGeneration,input.renderW,input.renderH);
  ScopedSrTransition<NVSDK_NGX_Parameter> transition(const_cast<NVSDK_NGX_Parameter*>(params),qualityTransition,p_GetI,p_GetF,SetVirtualI,SetVirtualF);
  if(transition.resetWritten)input.reset=1;
  if(qualityTransition)Log("DLSS SR quality transition: generation=%llu render=%ux%u gameReset=%d resetApplied=%d preExposure=%g repaired=%d exposureScale=%g repaired=%d",(unsigned long long)featureGeneration,input.renderW,input.renderH,transition.originalReset,transition.resetWritten,transition.originalPreExposure,transition.preExposureWritten,transition.originalExposureScale,transition.exposureScaleWritten);
  if(!isDlssg && params){
    if(cmd && input.valid){if(!PrepareFrameData(cmd,input,prepared))CommitRdrCamera(false);}
    else InvalidateRdrCameraHistory();
  }

  g_wdPos.store(11);
  NVSDK_NGX_Result r = o_NgxEvaluate(cmd,h,params,cb);
  transition.Restore();
  dlssActivity.Completed(GetTickCount64(),r==NGX_Success && input.valid);
  if(Cfg().provider==FGProvider::DLSSG && input.valid)g_srTransition.Commit(featureGeneration,input.renderW,input.renderH,r==NGX_Success);
  g_wdPos.store(12);
  if(Cfg().provider==FGProvider::DLSSG)CommitFrameData(cmd,prepared,r==NGX_Success);
  if(IsDX12Bridge(Cfg().provider)){
    // A recorded snapshot is accepted only after the game's SR evaluation
    // succeeds. Without this commit, ApplyNeutralCamera resets EVERY frame.
    const bool accepted=prepared.ready && r==NGX_Success;
    XessEvalResult(cmd,accepted);CommitRdrCamera(accepted);
  }

  // AFTER orig: SR wrote Output before final color processing; its optional tag defaults off.
  if(!isDlssg && r==NGX_Success && cmd && params && input.valid && p_GetVoidPointer){
    void* outPtr=nullptr;
    if(p_GetVoidPointer(const_cast<NVSDK_NGX_Parameter*>(params),"Output",&outPtr)==NGX_Success && outPtr){
      auto* res=reinterpret_cast<NVSDK_NGX_Resource_VK*>(outPtr);
      auto& iv=res->Resource.ImageViewInfo;
      if(iv.Image && iv.ImageView && iv.Format!=VK_FORMAT_UNDEFINED)
        TagHUDLessColor(prepared.ready && r==NGX_Success?prepared.index:0,cmd,iv.Image,iv.ImageView,iv.Format,iv.Width,iv.Height);
    }
  }
  g_inEval=false;
  g_wdPos.store(0);
  return r;
}

// Feature creation is transactional too. Flags belong to the returned SR
// handle, not a global last-created feature or a failed creation attempt.
static void RegisterSrFeature(unsigned id,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** handle,NVSDK_NGX_Result result){
  if(result!=NGX_Success || !handle || !*handle)return;
  int flags=-1;
  if(id==1 && params && p_GetI)p_GetI(params,"DLSS.Feature.Create.Flags",&flags);
  {std::lock_guard<std::mutex> lock(g_featureMutex);g_srFeatures.erase(*handle);if(flags>=0)g_srFeatures[*handle]={flags,++g_srGeneration};}
  if(flags>=0){
    g_srCreateFlags.store(flags);unsigned w=0,h=0,ow=0,oh=0;
    if(p_GetUI){p_GetUI(params,"Width",&w);p_GetUI(params,"Height",&h);p_GetUI(params,"OutWidth",&ow);p_GetUI(params,"OutHeight",&oh);}
    Log("FG input pipeline v2: accepted SR handle=%p flags=0x%x render=%ux%u output=%ux%u",(void*)*handle,flags,w,h,ow,oh);
  }
}
static NVSDK_NGX_Result __cdecl h_NgxCreateFeature(VkCommandBuffer cmd,unsigned id,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** handle){
  const auto result=o_NgxCreateFeature(cmd,id,params,handle);
  if(!IsGpuBlocked())RegisterSrFeature(id,params,handle,result);return result;
}
using GetProcFn=FARPROC (WINAPI*)(HMODULE,LPCSTR);
static NVSDK_NGX_Result __cdecl h_CreateFeature1(VkDevice device,VkCommandBuffer cmd,unsigned id,NVSDK_NGX_Parameter* params,NVSDK_NGX_Handle** handle){
  const auto result=originalCreateFeature1(device,cmd,id,params,handle);
  if(!IsGpuBlocked())RegisterSrFeature(id,params,handle,result);return result;
}
static GetProcFn g_realGetProc=nullptr;
static FARPROC WINAPI h_EarlyNgxGetProc(HMODULE module,LPCSTR name){
  FARPROC original=g_realGetProc(module,name);
  if(IsGpuBlocked() || !original || reinterpret_cast<uintptr_t>(name)<65536)return original;
  bool eval=strcmp(name,"NVSDK_NGX_VULKAN_EvaluateFeature")==0;
  bool create=strcmp(name,"NVSDK_NGX_VULKAN_CreateFeature")==0;
    bool create1=strcmp(name,"NVSDK_NGX_VULKAN_CreateFeature1")==0;
    if(!eval && !create && !create1)return original;
    wchar_t provider[MAX_PATH]{};GetModuleFileNameW(module,provider,MAX_PATH);
    const wchar_t* providerBase=wcsrchr(provider,L'\\');providerBase=providerBase?providerBase+1:provider;
    if(_wcsicmp(providerBase,L"_nvngx.dll")!=0 && _wcsicmp(providerBase,L"nvngx.dll")!=0)return original;
    if(_wcsnicmp(providerBase,L"sl.",3)==0 || _wcsnicmp(providerBase,L"nvngx_dlssg",11)==0)return original;
  if(!p_GetVoidPointer){
    forEachNgxModule([&](HMODULE m,const wchar_t*){
      auto getter=(PFN_NgxGetVoidPointer)g_realGetProc(m,"NVSDK_NGX_Parameter_GetVoidPointer");
      if(!getter)return false;p_GetVoidPointer=getter;
      p_GetF=(PFN_NgxGetF)g_realGetProc(m,"NVSDK_NGX_Parameter_GetF");
      p_GetUI=(PFN_NgxGetUI)g_realGetProc(m,"NVSDK_NGX_Parameter_GetUI");
      p_GetI=(PFN_NgxGetI)g_realGetProc(m,"NVSDK_NGX_Parameter_GetI");return true;
    });
  }
  if(!p_GetVoidPointer || !p_GetI)return original;
    if(create1){
      if(originalCreateFeature1 && originalCreateFeature1!=(CreateFeature1)original)return original;
      originalCreateFeature1=(CreateFeature1)original;return (FARPROC)h_CreateFeature1;
    }
  if(eval){
    if(o_NgxEvaluate && o_NgxEvaluate!=(PFN_NgxEvaluateFeature)original)return original;
    o_NgxEvaluate=(PFN_NgxEvaluateFeature)original;return (FARPROC)&h_NgxEvaluate;
  }
  if(o_NgxCreateFeature && o_NgxCreateFeature!=(PFN_NgxCreateFeature)original)return original;
  o_NgxCreateFeature=(PFN_NgxCreateFeature)original;return (FARPROC)&h_NgxCreateFeature;
}
bool InstallEarlyNgxHooks(){
  p_GetVoidPointer=GetVirtualVoid;p_GetF=GetVirtualF;p_GetUI=GetVirtualUI;p_GetI=GetVirtualI;
  g_realGetProc=&GetProcAddress;
  if(DetourTransactionBegin()!=NO_ERROR)return false;
  DetourUpdateThread(GetCurrentThread());
  LONG attach=DetourAttach(&(PVOID&)g_realGetProc,(PVOID)&h_EarlyNgxGetProc);
  if(attach!=NO_ERROR){DetourTransactionAbort();return false;}
  LONG commit=DetourTransactionCommit();Log("Early NGX export routing commit=%ld",commit);return commit==NO_ERROR;
}
NativeInputReset ResetNativeFGInputs(unsigned preset){
  // Lock order matches the SR producer: SR -> frame queue -> association.
  // Never block presentation waiting for a worker to finish recording.
  std::unique_lock<std::mutex> sr(g_srMutex,std::try_to_lock);
  if(!sr.owns_lock())return NativeInputReset::Busy;
  std::lock_guard<std::mutex> queue(g_qMutex);
  for(uint32_t i=0;i<g_qCount;++i)if(g_q[(g_qHead+i)%kQCap].recording)return NativeInputReset::Busy;
  const bool reset=RestartNativeFGResources(preset);
  for(auto& slot:g_q)if(slot.index)CancelRenderReprojection(slot.index);
  {std::lock_guard<std::mutex> association(g_associationMutex);g_association=FrameAssociation{};}
  for(auto& slot:g_q)slot=FrameSlot{};
  g_qHead=g_qCount=0;g_loadingPresent=FrameSlot{};
  g_presentIndex=0;g_presentHasInputs=g_presentQueued=g_presentReady=false;
  g_presentDecision={};g_history=FrameHistory{};g_evalSeen.store(false);
  ResetFrameCameraHistory();
  return reset?NativeInputReset::Complete:NativeInputReset::Failed;
}
void NgxProbeTick(){
  if(g_hooked) return;
  if(g_probeDelay-->0) return;
  g_probeDelay=120;
  HMODULE found{}; const wchar_t* foundName=nullptr;
  auto resolve=g_realGetProc?g_realGetProc:&GetProcAddress;
  forEachNgxModule([&](HMODULE m, const wchar_t* name){
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(m,path,MAX_PATH);const wchar_t* base=wcsrchr(path,L'\\');base=base?base+1:path;
    if(_wcsicmp(base,L"_nvngx.dll")!=0 && _wcsicmp(base,L"nvngx.dll")!=0)return false;
    if(!resolve(m,"NVSDK_NGX_VULKAN_EvaluateFeature")) return false;
    found=m; foundName=name; return true; });
  if(!found) return;
  if(g_hooked)return;
  o_NgxEvaluate=(PFN_NgxEvaluateFeature)resolve(found,"NVSDK_NGX_VULKAN_EvaluateFeature");
  o_NgxCreateFeature=(PFN_NgxCreateFeature)resolve(found,"NVSDK_NGX_VULKAN_CreateFeature");
  originalCreateFeature1=(CreateFeature1)resolve(found,"NVSDK_NGX_VULKAN_CreateFeature1");
  if(o_NgxEvaluate==h_NgxEvaluate || o_NgxCreateFeature==h_NgxCreateFeature)return;
  // param getters can live in a different module - probe independently
  forEachNgxModule([&](HMODULE m, const wchar_t*){
    auto gp=(PFN_NgxGetVoidPointer)resolve(m,"NVSDK_NGX_Parameter_GetVoidPointer");
    if(!gp) return false;
    p_GetVoidPointer=gp;
    p_GetF=(PFN_NgxGetF)resolve(m,"NVSDK_NGX_Parameter_GetF");
    p_GetUI=(PFN_NgxGetUI)resolve(m,"NVSDK_NGX_Parameter_GetUI");
    p_GetI=(PFN_NgxGetI)resolve(m,"NVSDK_NGX_Parameter_GetI");
    return true; });
  if(!p_GetVoidPointer || !p_GetF || !p_GetI || !p_GetUI)return;
  DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID&)o_NgxEvaluate,(PVOID)h_NgxEvaluate);
  if(o_NgxCreateFeature) DetourAttach(&(PVOID&)o_NgxCreateFeature,(PVOID)h_NgxCreateFeature);
  if(originalCreateFeature1)DetourAttach(&(PVOID&)originalCreateFeature1,(PVOID)h_CreateFeature1);
  LONG rc=DetourTransactionCommit();
  g_hooked=(rc==NO_ERROR);
  Log("NGX hooks: module=%S commit=%ld eval=%p create=%p getters=%p/%p/%p",
      foundName?foundName:L"?",rc,(void*)o_NgxEvaluate,(void*)o_NgxCreateFeature,
      (void*)p_GetVoidPointer,(void*)p_GetF,(void*)p_GetUI);
}

// ---- present-thread half of the marker ladder ------------------------------------------
// Present markers MUST be on the present thread with the frame's token: sl.reflex turns our
// ePresentStart into latency.markerPresentFrame, which is how DLSS-G finds this frame's
// tags+constants (guide 8.0: "markers ePresentStart/ePresentEnd must provide correct frame
// index so that it can be matched" to the constants).
void ForgetFGCommand(VkCommandBuffer cmd){if(IsDX12Bridge(Cfg().provider))XessForget(cmd);NativeHudForget(cmd);InputCaptureForget(cmd);RenderReprojectionForget(cmd);std::lock_guard<std::mutex> lock(g_associationMutex);g_association.Forget((uint64_t)(uintptr_t)cmd);}
void ExecuteFGCommands(VkCommandBuffer cmd,uint32_t n,const VkCommandBuffer* secondary){if(IsDX12Bridge(Cfg().provider))XessExecute(cmd,n,secondary);NativeHudExecute(cmd,n,secondary);InputCaptureExecuted(cmd,n,secondary);RenderReprojectionExecuted(cmd,n,secondary);std::vector<uint64_t> children;for(uint32_t i=0;i<n;++i)children.push_back((uint64_t)(uintptr_t)secondary[i]);std::lock_guard<std::mutex> lock(g_associationMutex);g_association.Execute((uint64_t)(uintptr_t)cmd,children);}
void RecordFGSubmission(VkQueue queue,uint32_t count,const VkSubmitInfo* submits){
 if(!submits || Cfg().provider!=FGProvider::DLSSG)return;
 std::lock_guard<std::mutex> lock(g_associationMutex);
 for(uint32_t i=0;i<count;++i){const auto& s=submits[i];std::vector<uint64_t> commands,signals,waits;
  for(uint32_t j=0;j<s.commandBufferCount;++j)commands.push_back((uint64_t)(uintptr_t)s.pCommandBuffers[j]);
  const VkTimelineSemaphoreSubmitInfo* timeline=nullptr;
  for(auto next=(const VkBaseInStructure*)s.pNext;next;next=next->pNext)if(next->sType==VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)timeline=(const VkTimelineSemaphoreSubmitInfo*)next;
  for(uint32_t j=0;j<s.waitSemaphoreCount;++j){if(timeline && j<timeline->waitSemaphoreValueCount && timeline->pWaitSemaphoreValues[j]!=0)continue;waits.push_back((uint64_t)(uintptr_t)s.pWaitSemaphores[j]);}
  for(uint32_t j=0;j<s.signalSemaphoreCount;++j){if(timeline && j<timeline->signalSemaphoreValueCount && timeline->pSignalSemaphoreValues[j]!=0)continue;signals.push_back((uint64_t)(uintptr_t)s.pSignalSemaphores[j]);}
  const auto token=g_association.Submit((uint64_t)(uintptr_t)queue,commands,waits,signals);
  NativeHudSubmit(s.commandBufferCount,s.pCommandBuffers,token);
 }
}
void RecordFGSubmission2(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits){
 if(!submits || Cfg().provider!=FGProvider::DLSSG)return;
 std::lock_guard<std::mutex> lock(g_associationMutex);
 for(uint32_t i=0;i<count;++i){const auto& s=submits[i];std::vector<uint64_t> commands,signals,waits;
  for(uint32_t j=0;j<s.commandBufferInfoCount;++j)commands.push_back((uint64_t)(uintptr_t)s.pCommandBufferInfos[j].commandBuffer);
  for(uint32_t j=0;j<s.waitSemaphoreInfoCount;++j)if(s.pWaitSemaphoreInfos[j].value==0)waits.push_back((uint64_t)(uintptr_t)s.pWaitSemaphoreInfos[j].semaphore);
  for(uint32_t j=0;j<s.signalSemaphoreInfoCount;++j)if(s.pSignalSemaphoreInfos[j].value==0)signals.push_back((uint64_t)(uintptr_t)s.pSignalSemaphoreInfos[j].semaphore);
  const auto token=g_association.Submit((uint64_t)(uintptr_t)queue,commands,waits,signals);
  for(uint32_t j=0;j<s.commandBufferInfoCount;++j)NativeHudSubmit(1,&s.pCommandBufferInfos[j].commandBuffer,token);
 }
}
bool PresentMarkersBegin(const VkPresentInfoKHR* present){
  g_presentReady=false;
  g_presentDecision=FrameHistory::Decision{};
  g_presentIndex=0;g_presentHasInputs=false;g_presentQueued=false;g_loadingPresent=FrameSlot{};
  uint32_t submittedFrame=0;
  if(present && present->pWaitSemaphores){std::vector<uint64_t> waits;for(uint32_t i=0;i<present->waitSemaphoreCount;++i)waits.push_back((uint64_t)(uintptr_t)present->pWaitSemaphores[i]);std::lock_guard<std::mutex> lock(g_associationMutex);submittedFrame=g_association.Present(waits);}

  auto& fns = GetSlFns();
  if(!fns.pclSetMarker) return false;
  std::lock_guard<std::mutex> lk(g_qMutex);
  static unsigned associated=0,corrected=0,unavailable=0;
  FrameSlot* exact=submittedFrame?QFind(submittedFrame):nullptr;
  if(exact && exact->submitted && !exact->presenting){
    ++associated;
    if(QFront()!=exact){
      ++corrected;
      if(corrected<=5 || corrected%300==0)Log("FG frame association corrected: queued=%u submitted=%u",QFront()?QFront()->index:0,submittedFrame);
      // A newer submission may reach present first. Never evict a slot still
      // recording its resource copies on another worker.
      for(uint32_t i=0;i<g_qCount;){
        const FrameSlot& old=g_q[(g_qHead+i)%kQCap];
        if(old.index==submittedFrame)break;
        if(!old.recording && !old.presenting)QRemove(old.index);else ++i;
      }
      exact=QFind(submittedFrame);
      g_history.Invalidate(FrameHistory::Association);
    }
  }else ++unavailable;
  if(DebugLoggingEnabled() && (associated+unavailable)%300==0)Log("FG frame association: directMatches=%u corrected=%u unavailable=%u",associated,corrected,unavailable);
  FrameSlot* f=exact && exact->submitted && !exact->recording && !exact->presenting?exact:nullptr;
  g_presentHasInputs=f!=nullptr;
  if(!g_presentHasInputs)g_history.Invalidate(FrameHistory::Missing);
  g_presentQueued=g_presentHasInputs;
  // Consume an unused placeholder during loading, otherwise its Streamline
  // token would remain in the queue until the provider recycled the handle.
  if(!f && QFront() && !QFront()->recording && !QFront()->submitted && !QFront()->presenting){f=QFront();g_presentQueued=true;}
  if(!f){
    if(!NewFrameSlot(g_loadingPresent))return false;
    f=&g_loadingPresent;
  }
  g_presentIndex=f->index;f->presenting=true;
  if(!TokenMatches(*f))return false;
  bool ready=g_presentHasInputs;
  if(f->submitted && !ready){
    static unsigned mismatches=0;
    if(++mismatches<=5 || mismatches%120==0)Log("FG present rejected mismatched inputs: presentToken=%u queuedToken=%u occurrences=%u",submittedFrame,f->index,mismatches);
  }
  if(ready){
    sl::Constants renderConstants{};
    g_presentDecision=g_history.Select(submittedFrame,f->index,(uint32_t)*f->tok,f->taggedIndex,f->constantsIndex);
    ready=f->haveConstants && fns.setConstants && g_presentDecision.valid;
    if(ready){
      renderConstants=f->constants;
      ResolveFrameCamera(f->index,renderConstants);
      if(Cfg().cameraSource==CameraSource::Derived || Cfg().cameraAudit){
        const auto unchanged=renderConstants;
        const bool fit=ResolveRenderReprojection(f->index,renderConstants,Cfg().cameraSource!=CameraSource::Derived);
        if(Cfg().cameraSource==CameraSource::Derived){if(!fit)renderConstants=unchanged;SetDerivedFrameResult(f->index,fit);}else renderConstants=unchanged;
      }
      // Apply present-time corrections to THIS frame before publishing its
      // constants. An evaluation-time flag alone arrives one frame too late.
      if(g_presentDecision.Reset())renderConstants.reset=sl::Boolean::eTrue;
      InputRecovered(RuntimeErrors::Code::FrameIndex);
    }else{
      char detail[240];snprintf(detail,sizeof(detail),"submitted=%u slot=%u SDK=%u tags=%u constants=%u lastPresented=%u; frame rejected",submittedFrame,f->index,(uint32_t)*f->tok,f->taggedIndex,f->constantsIndex,g_history.Last());
      InputError(RuntimeErrors::Code::FrameIndex,detail);g_history.Invalidate(FrameHistory::IndexMismatch);
    }
    if(ready){
      { static unsigned dumped=0; if(dumped<3){ ++dumped; const auto& k=renderConstants; auto badf=[](float v){ return !(v==v) || v>3.0e38f; };
        Log("FG constants dump: near=%g far=%g fov=%g aspect=%g mvInvalid=%g jitter=%.4f/%.4f mvScale=%.6f/%.6f reset=%d depthInv=%d camMotion=%d mv3d=%d ortho=%d dilated=%d jittered=%d bad[near=%d far=%d fov=%d aspect=%d mvInv=%d]",
          k.cameraNear,k.cameraFar,k.cameraFOV,k.cameraAspectRatio,k.motionVectorsInvalidValue,k.jitterOffset.x,k.jitterOffset.y,k.mvecScale.x,k.mvecScale.y,(int)k.reset,(int)k.depthInverted,(int)k.cameraMotionIncluded,(int)k.motionVectors3D,(int)k.orthographicProjection,(int)k.motionVectorsDilated,(int)k.motionVectorsJittered,badf(k.cameraNear),badf(k.cameraFar),badf(k.cameraFOV),badf(k.cameraAspectRatio),badf(k.motionVectorsInvalidValue)); } }
      const auto result=fns.setConstants(renderConstants,*f->tok,sl::ViewportHandle{0});
      ready=result==sl::Result::eOk;
      if(ready)InputRecovered(RuntimeErrors::Code::ConstantsFailed);
      else InputError(RuntimeErrors::Code::ConstantsFailed,"slSetConstants rejected input-derived render reprojection",(int)result);
      static unsigned contractLogs=0,resetLogs=0;
      const bool reset=renderConstants.reset==sl::Boolean::eTrue;
      const bool logFrame=++contractLogs<=5 || contractLogs%300==0;
      const bool logReset=reset && (++resetLogs<=10 || resetLogs%300==0);
      if(DebugLoggingEnabled() && (logFrame || logReset))Log("FG frame contract: submitted=%u SDK=%u tags=%u constants=%u markers=%u swapchain=%p image=%u reset=%d reasons=0x%x previous=%u setConstants=%d",submittedFrame,(uint32_t)*f->tok,f->taggedIndex,f->constantsIndex,f->index,present && present->swapchainCount && present->pSwapchains?(void*)present->pSwapchains[0]:nullptr,present && present->swapchainCount && present->pImageIndices?present->pImageIndices[0]:UINT32_MAX,reset,g_presentDecision.reasons,g_history.Last(),(int)result);
    }
    // Readiness is returned to the single gate owner. A transient fit failure
    // must not bypass its grace period and cause an immediate OFF/ON cycle.
  }
  NativeHudTag(present,*f->tok,ready?f->index:0);
  SimStart(*f); MidMarkers(*f);              // keep the ladder complete and ordered
  fns.pclSetMarker(sl::PCLMarker::eRenderSubmitEnd,*f->tok);
  fns.pclSetMarker(sl::PCLMarker::ePresentStart,*f->tok);   // -> latency.markerPresentFrame = this frame
  static bool tidChecked=false;
  if(!tidChecked && g_evalTid.load()){ tidChecked=true;
    uint32_t p=(uint32_t)GetCurrentThreadId(), e=g_evalTid.load();
    Log("threads: present tid=%lu, DLSS-SR eval tid=%lu%s", (unsigned long)p, (unsigned long)e,
        p==e ? " (same thread)" : " (DIFFERENT threads - token hand-off is cross-thread)"); }
  g_presentReady=ready;
  return ready;
}
void PresentMarkersEnd(VkResult result){
  auto& fns = GetSlFns();
  uint32_t nextIndex=0;
  { std::lock_guard<std::mutex> lk(g_qMutex);
    FrameSlot* presented=g_presentQueued?QFind(g_presentIndex):&g_loadingPresent;
    if(FrameSlot* f = presented){
      if(fns.pclSetMarker && f->tok && TokenMatches(*f)) fns.pclSetMarker(sl::PCLMarker::ePresentEnd,*f->tok);   // AFTER the real present
      if(g_presentHasInputs){
        g_history.Complete(g_presentDecision,g_presentReady,result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR);
        CommitRenderReprojection(f->index,g_presentReady && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR));
        CommitFrameCamera(f->index,g_presentReady && (result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR));
        {std::lock_guard<std::mutex> lock(g_associationMutex);g_association.Retire(f->index);}
      }
      if(g_presentQueued)QRemove(f->index);
    }
    g_presentIndex=0;g_presentHasInputs=false;g_presentQueued=false;
    // Next frame: reuse the slot a pipelined eval already opened, else open it here.
    FrameSlot* n = QFront(); if(!n) n = QPushNew();
    if(n)nextIndex=n->index; }
  if(!nextIndex)return;
  // Optional sleep must use a checked token too. Keep allocation serialized
  // until the SDK has consumed it; otherwise loading can recycle its handle.
  {std::lock_guard<std::mutex> lk(g_qMutex);
    if(FrameSlot* s=QFind(nextIndex);s && TokenMatches(*s)){
      if(kReflexSleep && fns.reflexSleep){ApplyNativeFrameBudget();double t0=NowMs();fns.reflexSleep(*s->tok);double d=NowMs()-t0;if(d>g_maxSleepMs)g_maxSleepMs=d;}
      SimStart(*s);
    }
  }
}
bool ConsumeEvalSeen(){ return g_evalSeen.exchange(false); }
bool HasRecentDLSSInput(){return dlssActivity.Recent(GetTickCount64(),Cfg().noDLSSGraceMs);}
uint64_t DLSSInputAgeMs(){return dlssActivity.Age(GetTickCount64());}

}

