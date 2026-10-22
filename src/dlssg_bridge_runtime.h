#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <array>
#include <cstdint>

namespace fgvk::dlssg_bridge {
struct RuntimeStatus { const char* operation=""; int64_t native{}; bool hardwareSchedulingRequired{}; };
struct PresentStatus {
  uint32_t numFramesActuallyPresented{};
  uint32_t status{};
  uint32_t numFramesToGenerateMax{};
  bool vsyncSupported{};
  HRESULT presentResult=S_OK;
  int64_t stateQueryResult{};
};
// All calls are serialized by the caller. GPU work must be retired before
// Shutdown. The only WSI owner for the HWND must be this DXGI presentation path.
class Runtime {
public:
  Runtime()=default;
  Runtime(const Runtime&)=delete;
  Runtime& operator=(const Runtime&)=delete;
  ~Runtime(){Shutdown();ReleaseBinding();}
  bool Initialize(ID3D12Device*);
  // The existing native queue is deliberately not used: SL must observe queue
  // creation through its upgraded device. Submit bridge work using Queue().
  bool CreatePresentation(HWND,ID3D12CommandQueue* existingNativeQueue,
      IDXGIFactory4*,const DXGI_SWAP_CHAIN_DESC1&,uint32_t maxGenerated,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen=nullptr,IDXGIOutput* output=nullptr);
  bool SetEnabled(bool,uint32_t generated);
  bool Sleep(uint64_t frameId,uint32_t fpsCap=0,uint32_t outputCap=0,uint32_t outputs=1);
  // Numeric values 0..5 match XeLL: simulation, submit, present start/end.
  bool Marker(uint64_t frameId,uint32_t marker);
  // All textures belong to nativeDevice and contain this exact frame. The
  // recording DIRECT list must execute on Queue() before Present. SL records
  // preservation copies for eOnlyValidNow. HUD-less is final postprocess color
  // before HUD; final color goes into Swapchain()->GetBuffer separately.
  bool TagFrame(ID3D12GraphicsCommandList*,uint64_t,const sl::Constants&,
      ID3D12Resource* hudless,ID3D12Resource* depth,ID3D12Resource* motionVectors,
      uint32_t colorWidth,uint32_t colorHeight,uint32_t inputWidth,uint32_t inputHeight,
      D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  HRESULT Present(UINT syncInterval,UINT flags);
  bool LastPresent(PresentStatus& out) const;
  uint32_t MaximumGeneratedFrames() const{return maximumGenerated_;}
  bool VSyncSupported() const{return haveState_ && last_.vsyncSupported;}
  ID3D12CommandQueue* Queue() const{return queue_;} // borrowed
  IDXGISwapChain3* Swapchain() const{return swapchain_;} // SL proxy, borrowed
  // Borrowed SL fence. Wait on a non-presenting queue before reusing inputs.
  ID3D12Fence* InputFence() const{return inputFence_;}
  uint64_t InputFenceValue() const{return inputFenceValue_;}
  const RuntimeStatus& Status() const{return status_;}
  // Retires this presentation instance; retains the process-wide SL device
  // binding so Initialize(sameDevice) can recreate a swapchain after resize.
  bool Shutdown();
  // Presentation and external swapchain references must already be retired.
  // Invalidates feature entry points; Initialize(sameDevice) refreshes them.
  bool RestartFeature();
private:
  struct Frame {uint64_t id{};sl::FrameToken* token{};uint32_t markers{};bool constants{};};
  Frame* GetFrame(uint64_t,bool create);
  bool Check(const char*,sl::Result);
  bool Fail(const char*,int64_t);
  bool PollState();
  bool ApplyReflex();
  void ReleaseBinding();
  ID3D12Device* device_{};
  ID3D12Device* proxyDevice_{};
  IDXGIFactory4* proxyFactory_{};
  ID3D12CommandQueue* queue_{};
  IDXGISwapChain3* swapchain_{};
  ID3D12Fence* inputFence_{};
  uint64_t inputFenceValue_{};
  std::array<Frame,8> frames_{};
  uint64_t taggedId_{},presentId_{},lastMarkerId_{};
  bool tagged_{},enabled_{},haveState_{},enabledKnown_{},presentPending_{},haveLastMarker_{},bindingReady_{};
  uint32_t maximumGenerated_{},requestedMaximum_{},generated_{1},lastMarker_{};
  uint32_t reflexFrameLimitUs_{};
  bool reflexActive_{};
  PresentStatus last_{};
  RuntimeStatus status_{};
  sl::ViewportHandle viewport_{0};
  PFun_slUpgradeInterface* upgrade_{};
  PFun_slSetD3DDevice* setDevice_{};
  PFun_slGetFeatureFunction* feature_{};
  PFun_slGetNewFrameToken* token_{};
  PFun_slSetTagForFrame* tags_{};
  PFun_slSetConstants* constants_{};
  PFun_slFreeResources* free_{};
  PFun_slDLSSGSetOptions* options_{};
  PFun_slDLSSGGetState* state_{};
  PFun_slReflexSetOptions* reflexOptions_{};
  PFun_slReflexSleep* sleep_{};
  PFun_slPCLSetMarker* marker_{};
};
}
namespace fgvk {using DlssgBridgeRuntime=dlssg_bridge::Runtime;}
