// Exercise the real production lifecycle with fake SDK callbacks. No GPU,
// driver SDK, injected module or game process is created.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <array>
#include <cstdint>
#include <vector>
#include <cstring>
#define private public
#include "../src/dlssg_bridge_runtime.h"
#undef private
#include "../src/dlssg_bridge_runtime.cpp"
#ifdef NDEBUG
#error Lifecycle fixtures require active assertions
#endif
#include <cassert>
#include <cstdio>
namespace {
std::vector<bool> loads;bool failLoad{},failUnload{},missing{},failReflex{};
unsigned reflexCalls{};sl::ReflexOptions observed;
sl::Result Load(sl::Feature f,bool enabled){
 assert(f==sl::kFeatureDLSS_G);loads.push_back(enabled);
 return (enabled?failLoad:failUnload)?sl::Result::eErrorIO:sl::Result::eOk;
}
sl::Result Reflex(const sl::ReflexOptions& options){++reflexCalls;observed=options;return failReflex?sl::Result::eErrorIO:sl::Result::eOk;}
struct Identity:IUnknown{
 unsigned releases{};
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out)override{*out=this;return S_OK;}
 ULONG STDMETHODCALLTYPE AddRef()override{return 2;}
 ULONG STDMETHODCALLTYPE Release()override{++releases;return 1;}
};
}
namespace fgvk{
void Log(const char*,...){}
bool EnsureStreamlineD3D12Init(){assert(false);return false;}
void* SlProxyFn(const char* name){assert(!std::strcmp(name,"slSetFeatureLoaded"));return missing?nullptr:reinterpret_cast<void*>(&Load);}

}
int main(){
 using fgvk::dlssg_bridge::Runtime;
 {Runtime fresh;assert(fresh.RestartFeature() && loads.empty());}
 Identity identity;
 for(unsigned i=0;i<100;++i){
  Runtime r;r.device_=reinterpret_cast<ID3D12Device*>(&identity);r.bindingReady_=true;
  r.enabled_=r.enabledKnown_=r.haveState_=true;r.last_.numFramesActuallyPresented=4;
  r.reflexOptions_=Reflex;r.reflexFrameLimitUs_=16666;
  r.swapchain_=reinterpret_cast<IDXGISwapChain3*>(uintptr_t(1));
  auto before=loads.size();assert(!r.RestartFeature() && loads.size()==before);r.swapchain_=nullptr;
  assert(r.RestartFeature() && loads.size()==before+2 && !loads[before] && loads[before+1]);
  assert(!r.bindingReady_ && !r.enabled_ && !r.enabledKnown_ && !r.haveState_ && !r.options_ && !r.state_);
  assert(r.device_==reinterpret_cast<ID3D12Device*>(&identity) && !r.last_.numFramesActuallyPresented);
  auto reflexBefore=reflexCalls;assert(r.ApplyReflex() && reflexCalls==reflexBefore+1);
  assert(observed.mode==sl::ReflexMode::eLowLatency && observed.frameLimitUs==16666);
 }
 assert(identity.releases==100);
 assert(observed.mode==sl::ReflexMode::eOff); // no retained Reflex during XeSS
 {Runtime r;r.device_=reinterpret_cast<ID3D12Device*>(&identity);r.bindingReady_=true;
  missing=true;assert(!r.RestartFeature() && r.bindingReady_);missing=false;
  failUnload=true;auto before=loads.size();assert(!r.RestartFeature() && loads.size()==before+1 && !loads.back());failUnload=false;
  failLoad=true;assert(!r.RestartFeature() && !r.bindingReady_ && !r.options_ && !r.state_);failLoad=false;
  assert(r.RestartFeature());
  r.reflexOptions_=Reflex;failReflex=true;assert(!r.ApplyReflex() && r.Status().native);failReflex=false;
 }
 puts("Production DLSS-G restart: 100 unload/reload cycles, live-presentation rejection, device retention, stale-state clearing, Reflex reapplication, missing/failed SDK callbacks and retry: PASS");
}
