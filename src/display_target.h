#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include "log.h"

namespace fgvk {
// Select by the game's HWND, never by output enumeration order, the primary
// flag, the slowest display, or the highest advertised display mode.
struct DisplayTarget {
 IDXGIOutput* output{};
 HMONITOR monitor{};
 DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen{};
 wchar_t name[CCHDEVICENAME]{};
 DisplayTarget(){fullscreen.Windowed=TRUE;fullscreen.ScanlineOrdering=DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;fullscreen.Scaling=DXGI_MODE_SCALING_UNSPECIFIED;}
 DisplayTarget(const DisplayTarget&)=delete;
 DisplayTarget& operator=(const DisplayTarget&)=delete;
 ~DisplayTarget(){if(output)output->Release();}
 void Resolve(HWND window,ID3D12Device* device,IDXGIFactory4* factory){
  monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};info.cbSize=sizeof(info);
  if(monitor && GetMonitorInfoW(monitor,&info)){
   wcsncpy_s(name,info.szDevice,_TRUNCATE);
   DEVMODEW mode{};mode.dmSize=sizeof(mode);
   if(EnumDisplaySettingsExW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode,0) && mode.dmDisplayFrequency>1){fullscreen.RefreshRate.Numerator=mode.dmDisplayFrequency;fullscreen.RefreshRate.Denominator=1;}
  }
  IDXGIAdapter1* adapter{};
  if(device && factory && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))){
   for(UINT i=0;;++i){IDXGIOutput* candidate{};auto hr=adapter->EnumOutputs(i,&candidate);if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))break;
    DXGI_OUTPUT_DESC desc{};
    if(SUCCEEDED(candidate->GetDesc(&desc)) && desc.AttachedToDesktop && desc.Monitor==monitor){output=candidate;break;}
    candidate->Release();
   }
   adapter->Release();
  }
  Log("Bridge display target: hwnd=%p monitor=%p device=%S refresh=%u/%u Hz matchedOutput=%d; windowed=1",window,monitor,name,fullscreen.RefreshRate.Numerator,fullscreen.RefreshRate.Denominator,output?1:0);
  // Hybrid GPU outputs may live on another adapter. Do not substitute output0
  // from the rendering adapter, force exclusive fullscreen, or change modes.
 }
};
inline void LogPresentationDisplay(HWND window,IDXGISwapChain3* swapchain){
 auto monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);IDXGIOutput* output{};
 if(!swapchain || FAILED(swapchain->GetContainingOutput(&output)) || !output){Log("Bridge display actual: GetContainingOutput unavailable hwnd=%p target=%p",window,monitor);return;}
 DXGI_OUTPUT_DESC desc{};auto hr=output->GetDesc(&desc);output->Release();
 if(SUCCEEDED(hr))Log("Bridge display actual: hwnd=%p windowMonitor=%p outputMonitor=%p device=%S matches=%d",window,monitor,desc.Monitor,desc.DeviceName,monitor==desc.Monitor?1:0);
}
}
