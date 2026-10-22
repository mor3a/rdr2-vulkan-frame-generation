#pragma once
#include "config.h"
#include "ampere_boot.h"
#include "xess_bridge_runtime.h"
#include "dlssg_bridge_runtime.h"
#include "fps_meter.h"
#include <algorithm>
#include <atomic>

namespace fgvk {
// One transport and one WSI owner. The selected backend supplies its own SDK,
// latency markers and presentation proxy; neither backend calls into the other.
class SharedBridgeRuntime {
  xess_bridge::Runtime xess_;
  DlssgBridgeRuntime dlss_;
  uint32_t generated_=1;
  bool enabled_{};
  std::atomic<FGProvider> selected_{FGProvider::Off};
public:
  // Caller retires the old backend before publishing a different selection.
  void Select(FGProvider provider){selected_.store(provider,std::memory_order_release);}
  FGProvider Selected()const{auto p=selected_.load(std::memory_order_acquire);return p==FGProvider::Off?Cfg().provider:p;}
  bool IsDLSS()const{return Selected()==FGProvider::DLSSGBridge;}
  // Follow with Initialize to refresh all feature entry points before creation.
  bool RefreshDLSSFeature(){return dlss_.RestartFeature();}
  bool RestoreStoppedXeSS(){return !IsDLSS() && xess_.RestoreAfterFailedDestroy();}
  bool Initialize(ID3D12Device* d,const wchar_t* path){
    generated_=1;enabled_=false;
    return IsDLSS()?(LoadAmpereUnlock() && dlss_.Initialize(d)):xess_.Initialize(d,path);
  }
  bool CreatePresentation(HWND h,ID3D12CommandQueue* q,IDXGIFactory4* f,
      const DXGI_SWAP_CHAIN_DESC1& desc,uint32_t flags,uint32_t,xefg_swapchain_ui_mode_t ui,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen=nullptr,IDXGIOutput* output=nullptr){
    if(IsDLSS())return dlss_.CreatePresentation(h,q,f,desc,5,fullscreen,output);
    return xess_.CreatePresentation(h,q,f,desc,flags,MaxInterpolations(),ui,fullscreen) && xess_.SetInterpolations(1);
  }
  ID3D12CommandQueue* Queue(ID3D12CommandQueue* original)const{return IsDLSS()?dlss_.Queue():original;}
  IDXGISwapChain3* Swapchain()const{return IsDLSS()?dlss_.Swapchain():xess_.Swapchain();}
  uint32_t MaxInterpolations()const{return IsDLSS()?dlss_.MaximumGeneratedFrames():(std::min)(3u,xess_.MaxInterpolations());}
  bool SetInterpolations(uint32_t count){
    if(count==generated_)return true;
    if(!count || count>MaxInterpolations())return false;
    if(!IsDLSS() && !xess_.SetInterpolations(count))return false;
    generated_=count;return true;
  }
  bool SetEnabled(bool enabled){bool ok=IsDLSS()?dlss_.SetEnabled(enabled,generated_):xess_.SetEnabled(enabled);if(ok)enabled_=enabled;return ok;}
  bool Sleep(uint32_t id,uint32_t fpsCap=0){const auto target=Rt().outputFPSCap.load();const auto outputs=enabled_?generated_+1:1;return IsDLSS()?dlss_.Sleep(id,fpsCap,target,outputs):xess_.Sleep(id,fpsCap,target,outputs);}
  bool VSyncSupported()const{return !IsDLSS() || dlss_.VSyncSupported();}
  bool Marker(uint32_t id,xell_latency_marker_type_t m){return IsDLSS()?dlss_.Marker(id,uint32_t(m)):xess_.Marker(id,m);}
  bool SetPresentId(uint32_t id){return IsDLSS() || xess_.SetPresentId(id);}
  bool TagFrame(uint32_t id,const xefg_swapchain_frame_constant_data_t& c,
      const xefg_swapchain_d3d12_resource_data_t* tags,uint32_t count,
      ID3D12GraphicsCommandList* list,const sl::Constants& slConstants){
    if(!IsDLSS())return xess_.TagFrame(id,c,tags,count,list);
    if(count!=3)return false;
    return dlss_.TagFrame(list,id,slConstants,tags[0].pResource,tags[1].pResource,tags[2].pResource,
      tags[0].resourceSize.x,tags[0].resourceSize.y,tags[1].resourceSize.x,tags[1].resourceSize.y);
  }
  HRESULT Present(UINT interval,UINT flags){return IsDLSS()?dlss_.Present(interval,flags):xess_.Present(interval,flags);}
  bool LastPresent(xefg_swapchain_present_status_t& out){
    if(!IsDLSS())return xess_.LastPresent(out);
    dlssg_bridge::PresentStatus state{};bool ok=dlss_.LastPresent(state);
    out={};out.framesPresented=state.numFramesActuallyPresented;
    out.isFrameGenEnabled=BridgeCountingActive(enabled_,state.status);
    out.frameGenResult=static_cast<xefg_swapchain_result_t>(state.status);
    return ok;
  }
  dlssg_bridge::RuntimeStatus Status()const{
    if(IsDLSS())return dlss_.Status();auto s=xess_.Status();return {s.operation,s.native};
  }
  int64_t StateQueryResult()const{if(IsDLSS()){dlssg_bridge::PresentStatus state{};dlss_.LastPresent(state);return state.stateQueryResult;}return xess_.Status().native;}
  ID3D12Fence* InputFence()const{return IsDLSS()?dlss_.InputFence():nullptr;}
  uint64_t InputFenceValue()const{return IsDLSS()?dlss_.InputFenceValue():0;}
  bool Shutdown(){return IsDLSS()?dlss_.Shutdown():xess_.Shutdown();}
};
}
