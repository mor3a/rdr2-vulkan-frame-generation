#include "dlssg_bridge_runtime.h"
#include "experiment_policy.h"
#include "slboot.h"
#include "sl_result_policy.h"
#include "runtime_errors.h"
#include <limits>
#include "log.h"

namespace fgvk {bool EnsureStreamlineD3D12Init();}
namespace fgvk::dlssg_bridge {
static_assert(static_cast<uint32_t>(sl::PCLMarker::eSimulationStart)==0 &&
  static_cast<uint32_t>(sl::PCLMarker::eSimulationEnd)==1 &&
  static_cast<uint32_t>(sl::PCLMarker::eRenderSubmitStart)==2 &&
  static_cast<uint32_t>(sl::PCLMarker::eRenderSubmitEnd)==3 &&
  static_cast<uint32_t>(sl::PCLMarker::ePresentStart)==4 &&
  static_cast<uint32_t>(sl::PCLMarker::ePresentEnd)==5,
  "Bridge marker contract must match the verified XeLL 0..5 values");
bool Runtime::Fail(const char* operation,int64_t value){status_={operation,value};return false;}
bool Runtime::Check(const char* operation,sl::Result result){
  if(!AcceptedSLResult(result))return Fail(operation,static_cast<int64_t>(result));
  if(result==sl::Result::eWarnOutOfVRAM)RuntimeErrors::SetWarning(RuntimeErrors::Warning::VRAM,true);
  status_={};return true;
}
bool Runtime::Initialize(ID3D12Device* device){
  if(!device)return Fail("Initialize/device",E_INVALIDARG);
  if(device_){
    IUnknown* oldIdentity{};IUnknown* newIdentity{};
    const auto oldResult=device_->QueryInterface(IID_PPV_ARGS(&oldIdentity));
    const auto newResult=device->QueryInterface(IID_PPV_ARGS(&newIdentity));
    const bool same=SUCCEEDED(oldResult) && SUCCEEDED(newResult) && oldIdentity==newIdentity;
    if(oldIdentity)oldIdentity->Release();if(newIdentity)newIdentity->Release();
    if(!same)return Fail("Initialize/different-SL-device",E_INVALIDARG);
    if(bindingReady_){status_={};return true;}
  }
  if(!EnsureStreamlineD3D12Init())return Fail("EnsureStreamlineD3D12Init",E_FAIL);
#define CORE(field,name) field=reinterpret_cast<decltype(field)>(SlProxyFn(name)); if(!field)return Fail(name,E_NOINTERFACE)
  CORE(upgrade_,"slUpgradeInterface"); CORE(setDevice_,"slSetD3DDevice");
  CORE(feature_,"slGetFeatureFunction"); CORE(token_,"slGetNewFrameToken");
  CORE(tags_,"slSetTagForFrame"); CORE(constants_,"slSetConstants");
  CORE(free_,"slFreeResources");
#undef CORE
  // slInit may succeed after discarding DLSS-G for an unmet OS requirement.
  // Ask the SDK before binding/initializing a device: looking up a function
  // afterwards reports only FeatureMissing (31), hiding the original HWS gate.
  auto supported=reinterpret_cast<PFun_slIsFeatureSupported*>(SlProxyFn("slIsFeatureSupported"));
  auto luid=device->GetAdapterLuid();sl::AdapterInfo adapter{};
  adapter.deviceLUID=reinterpret_cast<uint8_t*>(&luid);adapter.deviceLUIDSizeInBytes=sizeof(luid);
  const auto preflight=supported?supported(sl::kFeatureDLSS_G,adapter):sl::Result::eErrorMissingOrInvalidAPI;
  if(preflight==sl::Result::eErrorOSDisabledHWS){
    status_={"Enable Hardware-accelerated GPU scheduling in Windows Settings > System > Display > Graphics, then restart Windows. DLSS-G cannot start while HAGS is disabled.",int64_t(preflight),true};
    Log("DLSS-G OS prerequisite failed before device binding: HAGS unavailable; SL=%d adapter=%08lx:%08lx",int(preflight),luid.HighPart,luid.LowPart);
    return false;
  }
  // slSetD3DDevice initializes plugins and cannot be called again while the
  // process-wide SL instance remains initialized, even for the same device.
  if(!device_){
    if(!Check("slSetD3DDevice",setDevice_(device)))return false;
    device_=device;device_->AddRef();
  }
  if(!proxyDevice_){
    void* upgraded=device;
    if(!Check("slUpgradeInterface/device",upgrade_(&upgraded)))return false;
    proxyDevice_=static_cast<ID3D12Device*>(upgraded);
    // SL wrapper constructors own their own native reference. If interception
    // was disabled and the interface stayed native, acquire our reference.
    if(proxyDevice_==device)proxyDevice_->AddRef();
  }
  // A loaded DLL alone does not establish feature availability. Query the
  // actual DX12 adapter so a missing feature does not hide HAGS/driver errors.
  auto loaded=reinterpret_cast<PFun_slIsFeatureLoaded*>(SlProxyFn("slIsFeatureLoaded"));
  sl::Result support=sl::Result::eErrorFeatureMissing;
  if(supported)support=supported(sl::kFeatureDLSS_G,adapter);
  bool isLoaded=false;auto loadResult=loaded?loaded(sl::kFeatureDLSS_G,isLoaded):sl::Result::eErrorMissingOrInvalidAPI;
  Log("DLSS-G DX12 availability: support=%d loadedQuery=%d loaded=%d adapter=%08lx:%08lx",int(support),int(loadResult),int(isLoaded),luid.HighPart,luid.LowPart);
#define FEATURE(field,id,name) {void* fn{};auto result=feature_(id,name,fn);if(!AcceptedSLResult(result)){if(id==sl::kFeatureDLSS_G && result==sl::Result::eErrorFeatureMissing && supported && !AcceptedSLResult(support)){return Fail(support==sl::Result::eErrorOSDisabledHWS?"DLSS-G requires Windows hardware-accelerated GPU scheduling (HAGS); enable it and restart Windows":"DLSS-G unavailable on the selected DX12 adapter; see availability result",int64_t(support));}return Fail("slGetFeatureFunction/" name,int64_t(result));}field=reinterpret_cast<decltype(field)>(fn);if(!field)return Fail("slGetFeatureFunction/" name,E_NOINTERFACE);}
  FEATURE(options_,sl::kFeatureDLSS_G,"slDLSSGSetOptions");
  FEATURE(state_,sl::kFeatureDLSS_G,"slDLSSGGetState");
  FEATURE(reflexOptions_,sl::kFeatureReflex,"slReflexSetOptions");
  FEATURE(sleep_,sl::kFeatureReflex,"slReflexSleep");
  FEATURE(marker_,sl::kFeaturePCL,"slPCLSetMarker");
#undef FEATURE
  reflexFrameLimitUs_=0;
  bindingReady_=ApplyReflex();return bindingReady_;
}
bool Runtime::ApplyReflex(){
  if(!reflexOptions_)return Fail("slReflexSetOptions/unavailable",E_NOINTERFACE);
  sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatency;
  reflex.frameLimitUs=reflexFrameLimitUs_;
  const bool applied=Check("slReflexSetOptions/presentation",reflexOptions_(reflex));
  if(applied)reflexActive_=true;return applied;
}
bool Runtime::RestartFeature(){
  if(swapchain_ || queue_ || proxyFactory_)return Fail("RestartFeature/presentation-still-live",E_UNEXPECTED);
  if(!device_)return true; // First activation already takes full initialization.
  auto loaded=reinterpret_cast<PFun_slSetFeatureLoaded*>(SlProxyFn("slSetFeatureLoaded"));
  if(!loaded)return Fail("slSetFeatureLoaded/unavailable",E_NOINTERFACE);
  // slFreeResources frees viewport data, not all plugin-global presentation
  // state. The documented load API recreates that state without rebinding the
  // process-wide D3D device. No raw FreeLibrary or per-frame reset is used.
  if(!Check("slSetFeatureLoaded/DLSSG-restart-off",loaded(sl::kFeatureDLSS_G,false)))return false;
  bindingReady_=false;options_=nullptr;state_=nullptr;
  frames_={};last_={};tagged_=presentPending_=haveLastMarker_=haveState_=enabledKnown_=enabled_=false;
  inputFence_=nullptr;inputFenceValue_=0;maximumGenerated_=requestedMaximum_=0;generated_=1;
  if(!Check("slSetFeatureLoaded/DLSSG-restart-on",loaded(sl::kFeatureDLSS_G,true)))return false;
  Log("DLSS-G explicit restart: feature scheduler reloaded; device binding retained; feature functions and Reflex will be refreshed");
  return true;
}
bool Runtime::CreatePresentation(HWND hwnd,ID3D12CommandQueue*,IDXGIFactory4* factory,
    const DXGI_SWAP_CHAIN_DESC1& description,uint32_t maxGenerated,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,IDXGIOutput* output){
  if(!device_ || !proxyDevice_ || !options_ || !hwnd || !factory || swapchain_ || queue_ || !maxGenerated)
    return Fail("CreatePresentation/arguments",E_INVALIDARG);
  void* upgraded=factory;
  if(!Check("slUpgradeInterface/factory",upgrade_(&upgraded)))return false;
  proxyFactory_=static_cast<IDXGIFactory4*>(upgraded);
  if(proxyFactory_==factory)proxyFactory_->AddRef();
  D3D12_COMMAND_QUEUE_DESC queueDesc{};queueDesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
  HRESULT hr=proxyDevice_->CreateCommandQueue(&queueDesc,IID_PPV_ARGS(&queue_));
  if(FAILED(hr))return Fail("CreateCommandQueue/proxy",hr);
  // Leave pacing to SL Reflex rather than competing for its waitable object.
  if(description.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
    return Fail("CreatePresentation/frameLatencyWaitableObject",E_INVALIDARG);
  sl::DLSSGOptions off{};off.mode=sl::DLSSGMode::eOff;
  off.flags=sl::DLSSGFlags::eRetainResourcesWhenOff;
  if(!Check("slDLSSGSetOptions/off-before-create",options_(viewport_,off)))return false;
  enabledKnown_=true;enabled_=false;generated_=1;
  IDXGISwapChain1* initial{};
  hr=proxyFactory_->CreateSwapChainForHwnd(queue_,hwnd,&description,fullscreen,output,&initial);
  if(FAILED(hr))return Fail("CreateSwapChainForHwnd/proxy",hr);
  hr=initial->QueryInterface(IID_PPV_ARGS(&swapchain_));initial->Release();
  if(FAILED(hr))return Fail("QueryInterface/IDXGISwapChain3",hr);
  // Restore driver low latency for each presentation on the retained device.
  if(!ApplyReflex())return false;
  requestedMaximum_=maxGenerated<5?maxGenerated:5;
  if(!PollState())return false;
  // Capability is a provider result, never infer it from the GPU name.
  if(!maximumGenerated_)return Fail("DLSSG/numFramesToGenerateMax",E_FAIL);
  return true;
}
bool Runtime::SetEnabled(bool enabled,uint32_t generated){
  if(!options_ || !swapchain_ || (enabled && (!generated || generated>maximumGenerated_ || generated>requestedMaximum_)))
    return Fail("SetEnabled/arguments",E_INVALIDARG);
  const uint32_t count=enabled?generated:generated_;
  if(enabledKnown_ && enabled_==enabled && generated_==count){status_={};return true;}
  sl::DLSSGOptions options{};
  options.mode=enabled?sl::DLSSGMode::eOn:sl::DLSSGMode::eOff;
  options.numFramesToGenerate=count;
  options.flags=sl::DLSSGFlags::eRetainResourcesWhenOff;
  if(!Check("slDLSSGSetOptions",options_(viewport_,options)))return false;
  enabled_=enabled;enabledKnown_=true;generated_=count;return true;
}
Runtime::Frame* Runtime::GetFrame(uint64_t id,bool create){
  for(auto& frame:frames_)if(frame.token && frame.id==id)return &frame;
  if(!create){Fail("FrameToken/missing",E_INVALIDARG);return nullptr;}
  if(id>(std::numeric_limits<uint32_t>::max)()){Fail("FrameToken/id-overflow",E_INVALIDARG);return nullptr;}
  for(auto& frame:frames_)if(!frame.token){
    const uint32_t index=static_cast<uint32_t>(id);sl::FrameToken* token{};
    if(!Check("slGetNewFrameToken",token_(token,&index)) || !token)return nullptr;
    frame={id,token,0,false};return &frame;
  }
  Fail("FrameToken/outstanding-limit",E_FAIL);return nullptr;
}
bool Runtime::Sleep(uint64_t id,uint32_t fpsCap,uint32_t outputCap,uint32_t outputs){
  if(!sleep_ || !swapchain_)return Fail("Sleep/uninitialized",E_UNEXPECTED);
  // Limit at Reflex's frame-start boundary instead of delaying completed work
  // immediately before Present. Zero restores uncapped low-latency pacing.
  const uint32_t limitUs=FrameBudgetUs(fpsCap,outputCap,outputs);
  if(limitUs!=reflexFrameLimitUs_){
    sl::ReflexOptions reflex{};reflex.mode=sl::ReflexMode::eLowLatency;reflex.frameLimitUs=limitUs;
    if(!Check("slReflexSetOptions/frame-limit",reflexOptions_(reflex)))return false;
    reflexFrameLimitUs_=limitUs;
  }
  auto* frame=GetFrame(id,true);return frame && Check("slReflexSleep",sleep_(*frame->token));
}
bool Runtime::Marker(uint64_t id,uint32_t marker){
  if(!marker_ || !swapchain_ || marker>5)return Fail("Marker/arguments",E_INVALIDARG);
  // Repeated SR passes may revisit simulation/submit boundaries. Never replay
  // an already emitted boundary into the SDK, even after a later marker.
  if(haveLastMarker_ && lastMarkerId_==id && lastMarker_==marker)return true;
  auto* frame=GetFrame(id,true);if(!frame)return false;
  const uint32_t bit=1u<<marker;
  if(frame->markers&bit)return true;
  if(marker && !(frame->markers&(1u<<(marker-1))))
    return Fail("Marker/missing-predecessor",E_INVALIDARG);
  const auto priorDiagnostic=status_;
  if(!Check("slPCLSetMarker",marker_(static_cast<sl::PCLMarker>(marker),*frame->token)))return false;
  if(priorDiagnostic.native)status_=priorDiagnostic;
  frame->markers|=bit;
  lastMarkerId_=id;lastMarker_=marker;haveLastMarker_=true;
  if(marker==4){presentId_=id;presentPending_=true;}
  if(marker==5)*frame={};
  return true;
}
bool Runtime::TagFrame(ID3D12GraphicsCommandList* list,uint64_t id,const sl::Constants& constants,
    ID3D12Resource* hudless,ID3D12Resource* depth,ID3D12Resource* mvec,
    uint32_t cw,uint32_t ch,uint32_t iw,uint32_t ih,D3D12_RESOURCE_STATES resourceState){
  if(!swapchain_ || !list || !hudless || !depth || !mvec || !cw || !ch || !iw || !ih || tagged_)
    return Fail("TagFrame/arguments",E_INVALIDARG);
  if(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)
    return Fail("TagFrame/command-list-type",E_INVALIDARG);
  const auto colorDesc=hudless->GetDesc(),depthDesc=depth->GetDesc(),motionDesc=mvec->GetDesc();
  if(colorDesc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      depthDesc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      motionDesc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      colorDesc.Width<cw || colorDesc.Height<ch || depthDesc.Width<iw || depthDesc.Height<ih ||
      motionDesc.Width<iw || motionDesc.Height<ih || colorDesc.SampleDesc.Count!=1 ||
      depthDesc.SampleDesc.Count!=1 || motionDesc.SampleDesc.Count!=1)
    return Fail("TagFrame/resource-dimensions",E_INVALIDARG);
  auto* frame=GetFrame(id,true);if(!frame)return false;
  if(frame->constants)return Fail("TagFrame/duplicate-constants",E_INVALIDARG);
  if(!Check("slSetConstants",constants_(constants,*frame->token,viewport_)))return false;
  frame->constants=true;
  sl::Resource resources[]={
    {sl::ResourceType::eTex2d,hudless,static_cast<uint32_t>(resourceState)},
    {sl::ResourceType::eTex2d,depth,static_cast<uint32_t>(resourceState)},
    {sl::ResourceType::eTex2d,mvec,static_cast<uint32_t>(resourceState)}};
  sl::Extent colorExtent{0,0,cw,ch},inputExtent{0,0,iw,ih};
  sl::ResourceTag tags[]={
    {&resources[0],sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eOnlyValidNow,&colorExtent},
    {&resources[1],sl::kBufferTypeDepth,sl::ResourceLifecycle::eOnlyValidNow,&inputExtent},
    {&resources[2],sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eOnlyValidNow,&inputExtent}};
  if(!Check("slSetTagForFrame",tags_(*frame->token,viewport_,tags,3,reinterpret_cast<sl::CommandBuffer*>(list))))return false;
  taggedId_=id;tagged_=true;return true;
}
bool Runtime::PollState(){
  sl::DLSSGState state{};
  const auto result=state_(viewport_,state,nullptr);
  last_.stateQueryResult=static_cast<int64_t>(result);
  if(!Check("slDLSSGGetState",result)){haveState_=false;return false;}
  RuntimeErrors::SetWarning(RuntimeErrors::Warning::VRAM,result==sl::Result::eWarnOutOfVRAM);
  last_.numFramesActuallyPresented=state.numFramesActuallyPresented;
  last_.status=static_cast<uint32_t>(state.status);
  last_.numFramesToGenerateMax=state.numFramesToGenerateMax;
  last_.vsyncSupported=state.bIsVsyncSupportAvailable==sl::Boolean::eTrue;
  maximumGenerated_=state.numFramesToGenerateMax<5?state.numFramesToGenerateMax:5;
  if(requestedMaximum_ && maximumGenerated_>requestedMaximum_)maximumGenerated_=requestedMaximum_;
  inputFence_=static_cast<ID3D12Fence*>(state.inputsProcessingCompletionFence);
  inputFenceValue_=state.lastPresentInputsProcessingCompletionFenceValue;
  haveState_=true;return true;
}
HRESULT Runtime::Present(UINT sync,UINT flags){
  if(flags&DXGI_PRESENT_TEST){Fail("Present/test-not-a-frame",E_INVALIDARG);return E_INVALIDARG;}
  if(!swapchain_){Fail("Present/missing-swapchain",E_UNEXPECTED);return E_UNEXPECTED;}
  Frame* frame=nullptr;
  for(auto& candidate:frames_)if(candidate.token &&
      ((presentPending_ && candidate.id==presentId_) || (!presentPending_ && tagged_ && candidate.id==taggedId_)))frame=&candidate;
  RuntimeStatus preparationError=status_;
  const bool complete=frame && presentPending_ && (frame->markers&(1u<<4));
  if(!complete){Fail("Present/missing-latency-boundary",E_UNEXPECTED);preparationError=status_;}
  if(!complete || !tagged_ || taggedId_!=presentId_){
    // Missing same-frame inputs suspend FG for this frame, while native DXGI
    // presentation and normal latency markers continue through SL's proxy.
    if(!SetEnabled(false,generated_))preparationError=status_;
    sl::ResourceTag clear[]={
      {nullptr,sl::kBufferTypeHUDLessColor,sl::ResourceLifecycle::eValidUntilPresent},
      {nullptr,sl::kBufferTypeDepth,sl::ResourceLifecycle::eValidUntilPresent},
      {nullptr,sl::kBufferTypeMotionVectors,sl::ResourceLifecycle::eValidUntilPresent}};
    if(frame && !Check("slSetTagForFrame/clear-invalid",tags_(*frame->token,viewport_,clear,3,nullptr)))
      preparationError=status_;
  }
  // The caller must also use this API to select its actual rendering buffer.
  (void)swapchain_->GetCurrentBackBufferIndex();
  HRESULT hr=swapchain_->Present(sync,flags);tagged_=false;presentPending_=false;
  last_.presentResult=hr;
  if(FAILED(hr)){haveState_=false;Fail("IDXGISwapChain::Present",hr);return hr;}
  // State diagnostics never replace a successful DXGI result. LastPresent
  // returns false if this query failed; stateQueryResult retains the reason.
  if(PollState() && preparationError.native)status_=preparationError;
  // Incomplete frames cannot retain token slots indefinitely. A normal frame
  // is instead retired by its following PresentEnd marker.
  if(!complete)frames_={};
  return hr;
}
bool Runtime::LastPresent(PresentStatus& out) const{out=last_;return haveState_;}
bool Runtime::Shutdown(){
  bool ok=true;
  if(device_ && options_){sl::DLSSGOptions off{};off.mode=sl::DLSSGMode::eOff;ok=Check("slDLSSGSetOptions/shutdown",options_(viewport_,off));}
  if(device_ && free_ && state_)ok=Check("slFreeResources/shutdown",free_(sl::kFeatureDLSS_G,viewport_)) && ok;
  if(swapchain_){swapchain_->Release();swapchain_=nullptr;}
  if(queue_){queue_->Release();queue_=nullptr;}
  if(proxyFactory_){proxyFactory_->Release();proxyFactory_=nullptr;}
  // XeSS requires XeLL to be the only latency owner. A retained SL device
  // binding must not leave NVIDIA's driver sleep mode active after switching.
  if(reflexActive_ && reflexOptions_){
    sl::ReflexOptions off{};off.mode=sl::ReflexMode::eOff;
    const bool stopped=Check("slReflexSetOptions/provider-shutdown",reflexOptions_(off));
    if(stopped)reflexActive_=false;ok=stopped && ok;
  }
  frames_={};inputFence_=nullptr;inputFenceValue_=0;tagged_=false;enabled_=false;haveState_=false;
  enabledKnown_=false;presentPending_=false;haveLastMarker_=false;generated_=1;
  maximumGenerated_=requestedMaximum_=0;return ok;
}
void Runtime::ReleaseBinding(){
  if(proxyDevice_){proxyDevice_->Release();proxyDevice_=nullptr;}
  if(device_){device_->Release();device_=nullptr;}
  bindingReady_=false;
}
}
