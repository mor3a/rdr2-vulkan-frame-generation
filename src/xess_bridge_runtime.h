#pragma once
#include <windows.h>
#include <dxgi1_4.h>
#include <memory>
#include "../vendor/xemfg-reference/host_unlock.h"
#include "../vendor/intel-xess/inc/xell/xell_d3d12.h"
#include "../vendor/intel-xess/inc/xess_fg/xefg_swapchain_d3d12.h"

namespace fgvk::xess_bridge {
// Internal backend only. All calls must be serialized on the provider thread.
// The caller owns GPU retirement and all Vulkan/DX12 resource transitions.
enum class RuntimeError { None, InvalidCall, Library, Export, XeLL, XeFG, Dxgi, FrameContract };
struct RuntimeStatus {
  RuntimeError category{};
  const char* operation="";
  int64_t native{};
};
class Runtime {
public:
  Runtime()=default;
  Runtime(const Runtime&)=delete;
  Runtime& operator=(const Runtime&)=delete;
  ~Runtime(){Shutdown();}
  bool Initialize(ID3D12Device*,const wchar_t* absoluteRuntimeDirectory);
  // Use only after replacing native Vulkan WSI for this HWND. Never create
  // a second active presentation owner on top of the game's Vulkan swapchain.
  bool CreatePresentation(HWND,ID3D12CommandQueue*,IDXGIFactory2*,
      const DXGI_SWAP_CHAIN_DESC1&,uint32_t initFlags,uint32_t interpolations,
      xefg_swapchain_ui_mode_t,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen=nullptr);
  bool SetEnabled(bool);
  bool AttachPresentation(IDXGISwapChain3*,ID3D12CommandQueue*,uint32_t initFlags,
      uint32_t interpolations,xefg_swapchain_ui_mode_t);
  bool SetInterpolations(uint32_t);
  bool Sleep(uint32_t frameId,uint32_t fpsCap=0,uint32_t outputCap=0,uint32_t outputs=1);
  bool Marker(uint32_t frameId,xell_latency_marker_type_t);
  // Resources must be complete and associated with frameId. ONLY_NOW resources
  // require a recording DX12 copy list; execute that list before presentation.
  // Other resources must remain valid until the next present. A signal on the
  // app queue after Present alone does not retire XeFG's internal GPU queues.
  bool TagFrame(uint32_t frameId,const xefg_swapchain_frame_constant_data_t&,
      const xefg_swapchain_d3d12_resource_data_t*,uint32_t count,
      ID3D12GraphicsCommandList* resourceCopyList=nullptr);
  bool SetPresentId(uint32_t frameId);
  HRESULT Present(UINT syncInterval,UINT flags);
  bool LastPresent(xefg_swapchain_present_status_t&);
  bool Shutdown(); // GPU work retired; no outstanding proxy/backbuffer refs
  // Recover the SDK-owned presentation after -19, without releasing dependencies.
  bool RestoreAfterFailedDestroy();
  IDXGISwapChain3* Swapchain() const{return swapchain_;} // borrowed
  uint32_t MaxInterpolations() const{return properties_.maxSupportedInterpolations;}
  const RuntimeStatus& Status() const{return status_;}
  const xefg_swapchain_version_t& FGVersion() const{return fgVersion_;}
  const xell_version_t& LatencyVersion() const{return llVersion_;}
private:
  bool Fail(RuntimeError,const char*,int64_t);
  bool FG(const char*,xefg_swapchain_result_t);
  bool LL(const char*,xell_result_t);
  bool Load(HMODULE&,const wchar_t*,const wchar_t*);
  HMODULE fgModule_{},llModule_{};
  ID3D12Device* nativeDevice_{}; // Borrowed for strict software-pacer identity checks.
  std::unique_ptr<xess_host_unlock::Unlock> mfgUnlock_;
  bool mfgUnsafe_{}; // Keep SDK modules mapped if patch rollback is unverified.
  xefg_swapchain_handle_t fg_{};
  xell_context_handle_t ll_{};
  IDXGISwapChain3* swapchain_{};
  xefg_swapchain_properties_t properties_{};
  xefg_swapchain_version_t fgVersion_{};
  xell_version_t llVersion_{};
  RuntimeStatus status_{};
  uint32_t initializedMax_{},lastTagged_{};
  uint32_t minimumIntervalUs_{};
  bool tagged_{};
  bool enabled_{};
  bool enabledKnown_{};
#define XESS_FN(name) decltype(&name) p_##name{}
  XESS_FN(xellD3D12CreateContext); XESS_FN(xellDestroyContext);
  XESS_FN(xellSetSleepMode); XESS_FN(xellSleep); XESS_FN(xellAddMarkerData);
  XESS_FN(xellGetVersion);
  XESS_FN(xefgSwapChainD3D12CreateContext); XESS_FN(xefgSwapChainGetProperties);
  XESS_FN(xefgSwapChainSetLatencyReduction); XESS_FN(xefgSwapChainDestroy);
  XESS_FN(xefgSwapChainGetVersion); XESS_FN(xefgSwapChainD3D12InitFromSwapChainDesc);
  XESS_FN(xefgSwapChainD3D12InitFromSwapChain);
  XESS_FN(xefgSwapChainD3D12GetSwapChainPtr); XESS_FN(xefgSwapChainSetEnabled);
  XESS_FN(xefgSwapChainSetNumInterpolatedFrames); XESS_FN(xefgSwapChainSetUiCompositionState);
  XESS_FN(xefgSwapChainTagFrameConstants); XESS_FN(xefgSwapChainD3D12TagFrameResource);
  XESS_FN(xefgSwapChainSetPresentId); XESS_FN(xefgSwapChainGetLastPresentStatus);
#undef XESS_FN
};
}
