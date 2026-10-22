#include "xess_bridge_runtime.h"
#include "experiment_policy.h"
#include "log.h"
#include "xess_output_pacing.h"
#include <cwchar>
#include <cmath>

namespace fgvk::xess_bridge {
bool Runtime::Fail(RuntimeError category,const char* operation,int64_t native){
  status_={category,operation,native};return false;
}
bool Runtime::FG(const char* op,xefg_swapchain_result_t result){
  status_={result<0?RuntimeError::XeFG:RuntimeError::None,op,(int64_t)result};return result>=0;
}
bool Runtime::LL(const char* op,xell_result_t result){
  status_={result<0?RuntimeError::XeLL:RuntimeError::None,op,(int64_t)result};return result>=0;
}
bool Runtime::Load(HMODULE& out,const wchar_t* dir,const wchar_t* name){
  wchar_t combined[32768]{},absolute[32768]{},loaded[32768]{};
  if(!dir || !((dir[0] && dir[1]==L':' && (dir[2]==L'\\' || dir[2]==L'/')) ||
              (dir[0]==L'\\' && dir[1]==L'\\')))
    return Fail(RuntimeError::Library,"Runtime directory must be absolute",ERROR_BAD_PATHNAME);
  if(_snwprintf(combined,32768,L"%ls\\%ls",dir,name)<0)
    return Fail(RuntimeError::Library,"Runtime path too long",ERROR_FILENAME_EXCED_RANGE);
  DWORD size=GetFullPathNameW(combined,32768,absolute,nullptr);
  if(!size || size>=32768)return Fail(RuntimeError::Library,"GetFullPathNameW",GetLastError());
  // A different copy already loaded by another mod can satisfy dependent-DLL
  // imports even when our top-level LoadLibrary path is absolute. Refuse that
  // collision before introducing a second Intel runtime into the process.
  if(HMODULE existing=GetModuleHandleW(name)){
    size=GetModuleFileNameW(existing,loaded,32768);
    if(!size || size>=32768 || _wcsicmp(loaded,absolute))
      return Fail(RuntimeError::Library,"Another mod already loaded a different Intel runtime",ERROR_INVALID_DLL);
  }
  HMODULE module=LoadLibraryExW(absolute,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!module)return Fail(RuntimeError::Library,"Load Intel runtime",GetLastError());
  size=GetModuleFileNameW(module,loaded,32768);
  if(!size || size>=32768 || _wcsicmp(loaded,absolute)){
    FreeLibrary(module);return Fail(RuntimeError::Library,"Intel runtime path collision",ERROR_INVALID_DLL);
  }
  out=module;return true;
}
bool Runtime::Initialize(ID3D12Device* device,const wchar_t* directory){
  if(!device || fgModule_ || llModule_ || fg_ || ll_)
    return Fail(RuntimeError::InvalidCall,"Initialize requires a new backend and valid device",0);
  // Restore the original failure after unwinding a partially initialized SDK.
  auto unwind=[&](){auto failure=status_;Shutdown();status_=failure;return false;};
  if(!Load(llModule_,directory,L"libxell.dll") || !Load(fgModule_,directory,L"libxess_fg.dll"))return unwind();
#define RESOLVE(module,name) p_##name=(decltype(p_##name))GetProcAddress(module,#name); \
  if(!p_##name){Fail(RuntimeError::Export,#name,ERROR_PROC_NOT_FOUND);return unwind();}
  RESOLVE(llModule_,xellD3D12CreateContext);RESOLVE(llModule_,xellDestroyContext);
  RESOLVE(llModule_,xellSetSleepMode);RESOLVE(llModule_,xellSleep);
  RESOLVE(llModule_,xellAddMarkerData);RESOLVE(llModule_,xellGetVersion);
  RESOLVE(fgModule_,xefgSwapChainD3D12CreateContext);RESOLVE(fgModule_,xefgSwapChainGetProperties);
  RESOLVE(fgModule_,xefgSwapChainSetLatencyReduction);RESOLVE(fgModule_,xefgSwapChainDestroy);
  RESOLVE(fgModule_,xefgSwapChainGetVersion);RESOLVE(fgModule_,xefgSwapChainD3D12InitFromSwapChainDesc);
  RESOLVE(fgModule_,xefgSwapChainD3D12InitFromSwapChain);
  RESOLVE(fgModule_,xefgSwapChainD3D12GetSwapChainPtr);RESOLVE(fgModule_,xefgSwapChainSetEnabled);
  RESOLVE(fgModule_,xefgSwapChainSetNumInterpolatedFrames);RESOLVE(fgModule_,xefgSwapChainSetUiCompositionState);
  RESOLVE(fgModule_,xefgSwapChainTagFrameConstants);RESOLVE(fgModule_,xefgSwapChainD3D12TagFrameResource);
  RESOLVE(fgModule_,xefgSwapChainSetPresentId);RESOLVE(fgModule_,xefgSwapChainGetLastPresentStatus);
#undef RESOLVE
  if(!LL("xellGetVersion",p_xellGetVersion(&llVersion_)) ||
     !FG("xefgSwapChainGetVersion",p_xefgSwapChainGetVersion(&fgVersion_)))return unwind();
  // Pinned upstream MIT implementation verifies whole-function witnesses and
  // every patch site before writing. No context or GPU work exists yet.
  try{mfgUnlock_=std::make_unique<xess_host_unlock::Unlock>(fgModule_,llModule_,3,true);Log("XeSS MFG experimental unlock: validated maximum=3 generated frames (4x); upstream=2b52823e3769c31cf52d3b368c6963cff8fc5344");}
  catch(const xess_host_unlock::RollbackFailure&){mfgUnsafe_=true;return Fail(RuntimeError::FrameContract,"XeSS MFG patch rollback unverified; FG disabled and runtime modules retained",ERROR_INVALID_DATA);}
  catch(const std::exception& e){Log("XeSS MFG unlock not applied: %s; retaining native SDK limits",e.what());}
  // No SDK context/presentation worker exists yet. A refused compatibility
  // hook leaves the SDK's normal pacing untouched.
  xess_pacing::Install(fgModule_);nativeDevice_=device;
  if(!LL("xellD3D12CreateContext",p_xellD3D12CreateContext(device,&ll_)))return unwind();
  xell_sleep_params_t sleep{};sleep.bLowLatencyMode=1;
  if(!LL("xellSetSleepMode",p_xellSetSleepMode(ll_,&sleep)) ||
     !FG("xefgSwapChainD3D12CreateContext",p_xefgSwapChainD3D12CreateContext(device,&fg_)) ||
     !FG("xefgSwapChainSetLatencyReduction",p_xefgSwapChainSetLatencyReduction(fg_,ll_)) ||
     !FG("xefgSwapChainGetProperties",p_xefgSwapChainGetProperties(fg_,&properties_)))return unwind();
  if(!properties_.maxSupportedInterpolations){Fail(RuntimeError::XeFG,"XeSS FG exposes no interpolation capability",0);return unwind();}
  minimumIntervalUs_=0;
  return true;
}
bool Runtime::CreatePresentation(HWND window,ID3D12CommandQueue* queue,IDXGIFactory2* factory,
    const DXGI_SWAP_CHAIN_DESC1& desc,uint32_t flags,uint32_t count,xefg_swapchain_ui_mode_t uiMode,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen){
  if(!fg_ || swapchain_ || initializedMax_ || !window || !queue || !factory ||
     !desc.Width || !desc.Height || !count || count>MaxInterpolations() ||
     (flags&XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP))
    return Fail(RuntimeError::InvalidCall,"Invalid XeSS presentation initialization",0);
  xefg_swapchain_d3d12_init_params_t params{};
  params.initFlags=flags;params.maxInterpolatedFrames=count;params.uiMode=uiMode;
  if(!FG("xefgSwapChainD3D12InitFromSwapChainDesc",p_xefgSwapChainD3D12InitFromSwapChainDesc(
      fg_,window,&desc,fullscreen,queue,factory,&params)))return false;
  initializedMax_=count; // A later failure requires Shutdown, not a second init.
  if(!FG("xefgSwapChainD3D12GetSwapChainPtr",p_xefgSwapChainD3D12GetSwapChainPtr(
      fg_,__uuidof(IDXGISwapChain3),(void**)&swapchain_)))return false;
  if(!FG("xefgSwapChainSetUiCompositionState",p_xefgSwapChainSetUiCompositionState(fg_,
      uiMode==XEFG_SWAPCHAIN_UI_MODE_NONE?XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED:XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED)))return false;
  xess_pacing::Bind(fg_,nativeDevice_);
  return SetEnabled(false); // Enable only after a complete first-frame contract.
}
bool Runtime::SetEnabled(bool enabled){
  if(!swapchain_)return Fail(RuntimeError::InvalidCall,"SetEnabled before presentation initialization",0);
  if(enabledKnown_ && enabled_==enabled){status_={RuntimeError::None,"XeSS enabled state unchanged",0};return true;}
  if(!FG("xefgSwapChainSetEnabled",p_xefgSwapChainSetEnabled(fg_,enabled?1u:0u)))return false;
  xess_pacing::Reset(fg_);
  enabled_=enabled;enabledKnown_=true;return true;
}
bool Runtime::AttachPresentation(IDXGISwapChain3* existing,ID3D12CommandQueue* queue,uint32_t flags,
    uint32_t count,xefg_swapchain_ui_mode_t uiMode){
  if(!fg_ || swapchain_ || initializedMax_ || !existing || !queue || !count || count>MaxInterpolations())
    return Fail(RuntimeError::InvalidCall,"Invalid XeFG existing-swapchain initialization",0);
  xefg_swapchain_d3d12_init_params_t params{};params.pApplicationSwapChain=existing;
  params.initFlags=flags;params.maxInterpolatedFrames=count;params.uiMode=uiMode;
  if(!FG("xefgSwapChainD3D12InitFromSwapChain",p_xefgSwapChainD3D12InitFromSwapChain(fg_,queue,&params)))return false;
  initializedMax_=count;
  if(!FG("xefgSwapChainD3D12GetSwapChainPtr",p_xefgSwapChainD3D12GetSwapChainPtr(fg_,__uuidof(IDXGISwapChain3),(void**)&swapchain_)))return false;
  if(!FG("xefgSwapChainSetUiCompositionState",p_xefgSwapChainSetUiCompositionState(fg_,
      uiMode==XEFG_SWAPCHAIN_UI_MODE_NONE?XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_DISABLED:XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED)))return false;
  xess_pacing::Bind(fg_,nativeDevice_);return SetEnabled(false);
}
bool Runtime::SetInterpolations(uint32_t count){
  if(!swapchain_ || !count || count>initializedMax_)
    return Fail(RuntimeError::InvalidCall,"Unsupported XeSS interpolation count",count);
  if(!FG("xefgSwapChainSetNumInterpolatedFrames",p_xefgSwapChainSetNumInterpolatedFrames(fg_,count)))return false;
  xess_pacing::Reset(fg_);return true;
}
bool Runtime::Sleep(uint32_t id,uint32_t fpsCap,uint32_t outputCap,uint32_t outputs){
  if(!ll_ || !id)return Fail(RuntimeError::InvalidCall,"Invalid XeLL sleep frame",id);
  // XeLL owns the frame-start wait; cap there rather than holding a completed
  // application frame immediately before Present. Zero removes the cap.
  const uint32_t intervalUs=FrameBudgetUs(fpsCap,outputCap,outputs);
  if(intervalUs!=minimumIntervalUs_){
    xell_sleep_params_t sleep{};sleep.bLowLatencyMode=1;sleep.minimumIntervalUs=intervalUs;
    if(!LL("xellSetSleepMode/frame-limit",p_xellSetSleepMode(ll_,&sleep)))return false;
    minimumIntervalUs_=intervalUs;
  }
  return LL("xellSleep",p_xellSleep(ll_,id));
}
bool Runtime::Marker(uint32_t id,xell_latency_marker_type_t marker){
  if(!ll_ || !id || marker<0 || marker>=XELL_MARKER_COUNT)
    return Fail(RuntimeError::InvalidCall,"Invalid XeLL marker",id);
  return LL("xellAddMarkerData",p_xellAddMarkerData(ll_,id,marker));
}
bool Runtime::TagFrame(uint32_t id,const xefg_swapchain_frame_constant_data_t& constants,
    const xefg_swapchain_d3d12_resource_data_t* resources,uint32_t count,
    ID3D12GraphicsCommandList* resourceCopyList){
  if(!swapchain_ || !id || tagged_ || (lastTagged_ && int32_t(id-lastTagged_)<=0) ||
     !resources || !count || count>XEFG_SWAPCHAIN_RES_COUNT)
    return Fail(RuntimeError::FrameContract,"Missing, duplicate or unordered XeSS frame",id);
  bool seen[XEFG_SWAPCHAIN_RES_COUNT]{};
  for(uint32_t i=0;i<count;++i){
    const auto& r=resources[i];
    if(r.type<0 || r.type>=XEFG_SWAPCHAIN_RES_COUNT || seen[r.type] ||
       (r.type!=XEFG_SWAPCHAIN_RES_BACKBUFFER && !r.pResource) ||
       !r.resourceSize.x || !r.resourceSize.y ||
       (r.validity!=XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT &&
        (r.validity!=XEFG_SWAPCHAIN_RV_ONLY_NOW || !resourceCopyList)))
      return Fail(RuntimeError::FrameContract,"Invalid XeSS resource contract",i);
    seen[r.type]=true;
  }
  if(!seen[XEFG_SWAPCHAIN_RES_DEPTH] || !seen[XEFG_SWAPCHAIN_RES_MOTION_VECTOR] ||
     !seen[XEFG_SWAPCHAIN_RES_HUDLESS_COLOR])
    return Fail(RuntimeError::FrameContract,"XeSS requires matching depth, motion vectors and HUD-less color",id);
  for(float v:constants.viewMatrix)if(!std::isfinite(v))return Fail(RuntimeError::FrameContract,"Non-finite XeSS view matrix",id);
  for(float v:constants.projectionMatrix)if(!std::isfinite(v))return Fail(RuntimeError::FrameContract,"Non-finite XeSS projection matrix",id);
  if(!std::isfinite(constants.jitterOffsetX) || !std::isfinite(constants.jitterOffsetY) ||
     !std::isfinite(constants.motionVectorScaleX) || !std::isfinite(constants.motionVectorScaleY) ||
     !std::isfinite(constants.frameRenderTime) || constants.frameRenderTime<0)
    return Fail(RuntimeError::FrameContract,"Invalid XeSS frame constants",id);
  if(!FG("xefgSwapChainTagFrameConstants",p_xefgSwapChainTagFrameConstants(fg_,id,&constants)))return false;
  if(constants.resetHistory)xess_pacing::Reset(fg_);
  for(uint32_t i=0;i<count;++i)
    if(!FG("xefgSwapChainD3D12TagFrameResource",p_xefgSwapChainD3D12TagFrameResource(fg_,resourceCopyList,id,&resources[i])))return false;
  lastTagged_=id;tagged_=true;return true;
}
bool Runtime::SetPresentId(uint32_t id){
  if(!swapchain_ || !id || (enabled_ && (!tagged_ || id!=lastTagged_)))
    return Fail(RuntimeError::FrameContract,"XeSS present ID differs from current-frame tags",id);
  return FG("xefgSwapChainSetPresentId",p_xefgSwapChainSetPresentId(fg_,id));
}
HRESULT Runtime::Present(UINT interval,UINT flags){
  if(!swapchain_ || (enabled_ && !tagged_)){Fail(RuntimeError::FrameContract,"Enabled present requires a complete XeSS frame",0);return DXGI_ERROR_INVALID_CALL;}
  if(interval>4 || (flags&~DXGI_PRESENT_ALLOW_TEARING) || (interval && (flags&DXGI_PRESENT_ALLOW_TEARING))){
    Fail(RuntimeError::InvalidCall,"Unsupported XeSS presentation flags or interval",flags);return DXGI_ERROR_INVALID_CALL;
  }
  HRESULT result=swapchain_->Present(interval,flags);
  tagged_=false;
  if(FAILED(result))Fail(RuntimeError::Dxgi,"IDXGISwapChain::Present",result);
  return result;
}
bool Runtime::LastPresent(xefg_swapchain_present_status_t& result){
  if(!swapchain_)return Fail(RuntimeError::InvalidCall,"LastPresent before initialization",0);
  result={};return FG("xefgSwapChainGetLastPresentStatus",p_xefgSwapChainGetLastPresentStatus(fg_,&result));
}
bool Runtime::RestoreAfterFailedDestroy(){
  if(!fg_ || swapchain_ || mfgUnsafe_ || status_.native!=XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE || !p_xefgSwapChainD3D12GetSwapChainPtr)return false;
  const auto refused=status_;
  const auto result=p_xefgSwapChainD3D12GetSwapChainPtr(fg_,__uuidof(IDXGISwapChain3),reinterpret_cast<void**>(&swapchain_));
  if(result<0 || !swapchain_)return Fail(RuntimeError::XeFG,"Restore native presentation after refused XeSS destroy",result);
  tagged_=false;lastTagged_=0;enabled_=false;enabledKnown_=false;
  // Shutdown unbound output pacing. Keep generation disabled with a visible
  // error until a subsequent successful restart; no stale timing is resumed.
  status_=refused;return true;
}
bool Runtime::Shutdown(){
  if(fg_)xess_pacing::Unbind(fg_);
  if(mfgUnsafe_)return Fail(RuntimeError::FrameContract,"XeSS MFG patch rollback unverified; runtime modules retained",ERROR_INVALID_DATA);
  if(swapchain_){swapchain_->Release();swapchain_=nullptr;}
  if(fg_){
    auto result=p_xefgSwapChainDestroy(fg_);
    if(result<0)return Fail(RuntimeError::XeFG,"xefgSwapChainDestroy (SDK retained)",result);
    fg_=nullptr;
  }
  if(ll_){
    auto result=p_xellDestroyContext(ll_);
    if(result<0)return Fail(RuntimeError::XeLL,"xellDestroyContext (SDK retained)",result);
    ll_=nullptr;
  }
  if(mfgUnlock_){try{mfgUnlock_->Restore();}catch(const std::exception& e){return Fail(RuntimeError::FrameContract,"XeSS MFG restoration failed; SDK modules retained",ERROR_INVALID_DATA);}mfgUnlock_.reset();}
  if(fgModule_)FreeLibrary(fgModule_);if(llModule_)FreeLibrary(llModule_);
  fgModule_=llModule_=nullptr;properties_={};initializedMax_=lastTagged_=0;tagged_=enabled_=enabledKnown_=false;
  minimumIntervalUs_=0;
  nativeDevice_=nullptr;
  return true;
}
}
