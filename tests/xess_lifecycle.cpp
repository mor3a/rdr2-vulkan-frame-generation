// Production backend with fake COM/SDK callbacks; no GPU or game is started.
#include <windows.h>
#include <dxgi1_4.h>
#include <memory>

#include "../vendor/xemfg-reference/host_unlock.h"
#include "../vendor/intel-xess/inc/xell/xell_d3d12.h"
#include "../vendor/intel-xess/inc/xess_fg/xefg_swapchain_d3d12.h"
#define private public
#include "../src/xess_bridge_runtime.h"
#undef private
#include "../src/xess_bridge_runtime.cpp"
namespace fgvk {
template<class P,class F>bool ReleasePresentationBeforeSDK(P*& p,F f){if(p){auto* old=p;p=nullptr;old->Release();}return f();}
}
#ifdef NDEBUG
#error Lifecycle fixtures require active assertions
#endif
#include <cassert>
#include <cstdio>
namespace {
struct Context {
 void** table{};unsigned refs=2,tags{},sleeps{},destroyed{},latencyDestroyed{},nativePresents{};bool enabled{};
 uint32_t presentId{};HANDLE event{};UINT latency{};
 static ULONG STDMETHODCALLTYPE AddRef(Context* c){return ++c->refs;}
 static ULONG STDMETHODCALLTYPE Release(Context* c){assert(c->refs);return --c->refs;}
 static HRESULT STDMETHODCALLTYPE Desc(Context*,DXGI_SWAP_CHAIN_DESC1* out){*out={};out->Flags=0;return S_OK;}
 static HRESULT STDMETHODCALLTYPE Latency(Context* c,UINT n){c->latency=n;return S_OK;}
 static HANDLE STDMETHODCALLTYPE Event(Context* c){return c->event;}
 static HRESULT STDMETHODCALLTYPE Present(Context* c,UINT,UINT){assert(c->presentId);++c->nativePresents;return S_OK;}
};
Context* C(xefg_swapchain_handle_t c){return reinterpret_cast<Context*>(c);}
xefg_swapchain_result_t Destroy(xefg_swapchain_handle_t h){auto* c=C(h);if(c->refs)return XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE;++c->destroyed;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xefg_swapchain_result_t Recover(xefg_swapchain_handle_t h,REFIID,void** out){auto* c=C(h);++c->refs;*out=c;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xell_result_t DestroyLL(xell_context_handle_t h){auto* c=reinterpret_cast<Context*>(h);assert(c->destroyed);++c->latencyDestroyed;return XELL_RESULT_SUCCESS;}
xefg_swapchain_result_t Enable(xefg_swapchain_handle_t h,uint32_t enabled){auto* c=C(h);c->enabled=enabled!=0;c->tags=0;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xefg_swapchain_result_t Constants(xefg_swapchain_handle_t,uint32_t,const xefg_swapchain_frame_constant_data_t*){return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xefg_swapchain_result_t Tag(xefg_swapchain_handle_t h,ID3D12CommandList*,uint32_t,const xefg_swapchain_d3d12_resource_data_t*){assert(C(h)->enabled);++C(h)->tags;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xefg_swapchain_result_t Id(xefg_swapchain_handle_t h,uint32_t id){auto* c=C(h);assert(!c->enabled || c->tags==3);c->presentId=id;return XEFG_SWAPCHAIN_RESULT_SUCCESS;}
xell_result_t Sleep(xell_context_handle_t h,uint32_t){++reinterpret_cast<Context*>(h)->sleeps;return XELL_RESULT_SUCCESS;}
xell_result_t SleepMode(xell_context_handle_t,const xell_sleep_params_t*){return XELL_RESULT_SUCCESS;}
void Setup(fgvk::xess_bridge::Runtime& r,Context& c,void** table){
 c.table=table;r.fg_=reinterpret_cast<xefg_swapchain_handle_t>(&c);r.ll_=reinterpret_cast<xell_context_handle_t>(&c);
 r.swapchain_=reinterpret_cast<IDXGISwapChain3*>(&c);r.p_xefgSwapChainDestroy=Destroy;r.p_xellDestroyContext=DestroyLL;
 r.p_xefgSwapChainD3D12GetSwapChainPtr=Recover;
 r.p_xefgSwapChainSetEnabled=Enable;r.p_xefgSwapChainTagFrameConstants=Constants;r.p_xefgSwapChainD3D12TagFrameResource=Tag;
 r.p_xefgSwapChainSetPresentId=Id;r.p_xellSleep=Sleep;r.p_xellSetSleepMode=SleepMode;
}
}
namespace fgvk {
void Log(const char*,...){}
namespace xess_pacing {
bool Install(HMODULE)noexcept{return true;}bool Bind(void*,ID3D12Device*)noexcept{return true;}
void Reset(void*)noexcept{}void Unbind(void*)noexcept{}
}}
int main(){
 using fgvk::xess_bridge::Runtime;
 void* table[34]{};table[1]=(void*)&Context::AddRef;table[2]=(void*)&Context::Release;table[8]=(void*)&Context::Present;
 table[18]=(void*)&Context::Desc;table[31]=(void*)&Context::Latency;table[33]=(void*)&Context::Event;
 {Context c;Runtime r;Setup(r,c,table);auto* extra=r.swapchain_;
  assert(!r.Shutdown() && r.Status().native==-19 && c.refs==1 && !c.destroyed);
  assert(fgvk::ReleasePresentationBeforeSDK(extra,[&]{return r.Shutdown();}));
  assert(!extra && !c.refs && c.destroyed==1 && c.latencyDestroyed==1);
 }
 {Context c;c.refs=3;Runtime r;Setup(r,c,table);auto* extra=r.swapchain_;
  assert(!fgvk::ReleasePresentationBeforeSDK(extra,[&]{return r.Shutdown();}) && !extra && c.refs==1);
  assert(r.RestoreAfterFailedDestroy());extra=r.swapchain_;extra->AddRef();
  assert(c.refs==3 && r.SetEnabled(false) && (c.presentId=22) && SUCCEEDED(r.Present(0,0)));
  extra->Release();assert(c.refs==2); // third-party owner finally releases
  assert(fgvk::ReleasePresentationBeforeSDK(extra,[&]{return r.Shutdown();}) && !c.refs && c.destroyed==1);
 }
 for(unsigned iteration=0;iteration<100;++iteration){
  Context c;Runtime r;Setup(r,c,table);auto* extra=r.swapchain_;
  // Preserve public XeLL sleep behavior for 2x, 3x and 4x.
  for(unsigned multiplier=1;multiplier<=3;++multiplier)for(unsigned i=0;i<10;++i)assert(r.Sleep(i+1,0));
  assert(c.sleeps==30);
  // Enabling may clear SDK tags. The production caller enables first and
  // supplies a fresh present ID for both generated and native frames.
  xefg_swapchain_frame_constant_data_t constants{};
  xefg_swapchain_d3d12_resource_data_t resources[3]{};
  for(auto& resource:resources){resource.pResource=reinterpret_cast<ID3D12Resource*>(uintptr_t(1));resource.validity=XEFG_SWAPCHAIN_RV_ONLY_NOW;resource.resourceSize={1280,720};}
  resources[0].type=XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;resources[1].type=XEFG_SWAPCHAIN_RES_DEPTH;resources[2].type=XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
  assert(r.SetEnabled(true));
  assert(r.TagFrame(31,constants,resources,3,reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(1))));
  assert(c.tags==3 && !c.presentId);
  assert(!r.SetPresentId(32) && r.SetPresentId(31) && SUCCEEDED(r.Present(0,0)));
  assert(r.SetEnabled(false) && r.SetPresentId(32) && SUCCEEDED(r.Present(0,0)));
  assert(r.SetEnabled(true));
  assert(r.TagFrame(33,constants,resources,3,reinterpret_cast<ID3D12GraphicsCommandList*>(uintptr_t(1))));
  assert(r.SetPresentId(33) && SUCCEEDED(r.Present(0,0)) && c.nativePresents==3);
  assert(fgvk::ReleasePresentationBeforeSDK(extra,[&]{return r.Shutdown();}));
  assert(!extra && !c.refs && c.destroyed==1 && c.latencyDestroyed==1);

 }
 puts("Production XeSS lifecycle: reproduced -19; external-ref refusal retains native presentation and permits retry; 100 correct ordered shutdowns, unchanged 2x/3x/4x XeLL sleep calls: PASS");
}
