#include "xess_output_pacing.h"
#include "log.h"
#include "../vendor/xemfg-reference/runtime_layout.h"
#include "../vendor/xemfg-reference/sdk_pacing_dlssg.h"
#include <detours.h>
#include <intrin.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <limits>
#include <array>
#include <dxgi1_4.h>

namespace fgvk::xess_pacing {
namespace {
using Deadline=int64_t*(*)(void*,int64_t*,const void*,const void*,uint32_t,uint32_t);
using Present=HRESULT(*)(void*,UINT,UINT,uint64_t,const DXGI_PRESENT_PARAMETERS*,ID3D12Resource* const*,bool);
using Pace=bool(*)(void*,const void*,uint8_t,const void*,uint32_t);
Deadline original{};
Present originalPresent{};Pace nativePace{};
HMODULE providerModule{};
uintptr_t returnSite{},implementationTable{},chainTable{},getter{};
uintptr_t extraReturnSite{};
std::mutex installMutex;
struct Owner {
 void* context{};uintptr_t implementation{},chain{},presenter{},native{},device{};
 std::atomic<bool> retired{false};std::atomic<unsigned> diagnostics{0};
 std::mutex mutex;sdk_pacing::cadence::BatchIntervalEstimator clock;
};
std::shared_ptr<Owner> owner;
bool Read(uintptr_t address,void* output,size_t size) noexcept {
 if(address<0x10000 || !output || !size || size>512 || address>UINTPTR_MAX-size)return false;
 SIZE_T read{};return ReadProcessMemory(GetCurrentProcess(),(const void*)address,output,size,&read) && read==size;
}
template<class T> bool Value(uintptr_t address,T& out) noexcept {return Read(address,&out,sizeof(out));}
bool Stack(uintptr_t address,size_t size) noexcept {
 const auto* tib=reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
 return address>=uintptr_t(tib->StackLimit) && address<=uintptr_t(tib->StackBase) && size<=uintptr_t(tib->StackBase)-address;
}
int64_t NowNs() noexcept {
 LARGE_INTEGER q{},f{};
 if(!QueryPerformanceCounter(&q) || !QueryPerformanceFrequency(&f) || q.QuadPart<0 || f.QuadPart<=0 ||
    f.QuadPart>INT64_MAX/1000000000 || q.QuadPart/f.QuadPart>INT64_MAX/1000000000)return 0;
 return (q.QuadPart/f.QuadPart)*1000000000+(q.QuadPart%f.QuadPart)*1000000000/f.QuadPart;
}
bool Context(const Owner& o) noexcept {
 uint32_t magic{};uintptr_t impl{},chain{},table{},fn{};
 return Value(uintptr_t(o.context),magic) && magic==0x47466558 &&
  Value(uintptr_t(o.context)+8,impl) && impl==o.implementation &&
  Value(impl,table) && table==implementationTable && Value(table+0x80,fn) && fn==getter &&
  Value(impl+8,chain) && chain==o.chain && Value(chain,table) && table==chainTable;
}
bool Live(const Owner& o) noexcept {
 if(o.retired.load(std::memory_order_acquire) || !Context(o))return false;
 uintptr_t presenter{},native{},device{};uint8_t intel{},hardware{},software{};
 return Value(o.chain+0x120,presenter) && presenter==o.presenter &&
  Value(o.chain+0x30,native) && native==o.native &&
  Value(o.presenter+0x18,native) && native==o.native &&
  Value(o.presenter,device) && device==o.device &&
  Value(o.presenter+0x64,intel) && intel==0 &&
  Value(o.presenter+0x340,hardware) && hardware==0 &&
  Value(o.presenter+0x341,software) && software==1;
}
__declspec(noinline) int64_t* Hook(void* timing,int64_t* output,const void* record,const void* summary,uint32_t index,uint32_t total) {
 const auto caller=uintptr_t(_ReturnAddress());
 auto* result=original(timing,output,record,summary,index,total);
 const DWORD error=GetLastError();
 if(result==output && caller==returnSite && index>=1 && total>=2 && total<=4 && index<total){
  try {
   auto current=std::atomic_load_explicit(&owner,std::memory_order_acquire);
   if(current && uintptr_t(timing)==current->presenter+0x168 && Stack(uintptr_t(output),sizeof(*output)) &&
      Stack(uintptr_t(record),0x28) && Live(*current)){
    uint32_t frame{};int64_t timestamp{},batchStart{};
    const int64_t now=NowNs();
    if(Value(uintptr_t(record),timestamp) && timestamp>0 &&
       Value(uintptr_t(record)+0x10,frame) && Value(uintptr_t(record)+0x20,batchStart) &&
       now>0 && now<=INT64_MAX-1000000000 && batchStart>0 &&
       batchStart>=now-250000000 && batchStart<=now+250000000){
     sdk_pacing::cadence::BatchIntervalEstimator::Decision plan{};
     std::lock_guard<std::mutex> lock(current->mutex);
     if(!current->retired.load(std::memory_order_acquire)){
      if(index==1)current->clock.Observe(frame,total,batchStart);
      if(current->clock.Plan(frame,total,index,now,plan)){
       *output=plan.deadline;
       const unsigned diagnostic=current->diagnostics.fetch_add(1,std::memory_order_relaxed);
       if(diagnostic==0)Log("XeSS output pacing: first owned deadline adjusted; outputs=%u intervalNs=%lld",total,(long long)plan.interval);
       else if(DebugLoggingEnabled() && diagnostic<12)
        LogDebug("XeSS output pacing: frame=%u index=%u/%u intervalNs=%lld samples=%u capped=%d",frame,index,total,(long long)plan.interval,plan.samples,(int)plan.capped);
      }
     }
    }
   }
  }catch(...){} // Compatibility adjustment never replaces the SDK call/result.
 }
 SetLastError(error);return result;
}
// Unlocked MFG has intermediate generated outputs for which the stock SDK
// omits its Pace call. Keep the SDK's own wait helper; supply only that missing
// call, gated by the witnessed presenter stack contract and exact owner.
__declspec(noinline) HRESULT HookPresent(void* presenter,UINT sync,UINT flags,uint64_t frame,
 const DXGI_PRESENT_PARAMETERS* parameters,ID3D12Resource* const* resource,bool generated){
 const DWORD error=GetLastError();
 const auto caller=uintptr_t(_ReturnAddress()),slot=uintptr_t(_AddressOfReturnAddress());
 if(generated && !sync && !(flags&~UINT(DXGI_PRESENT_ALLOW_TEARING)) && caller==extraReturnSite &&
    !(slot&7) && Stack(slot,0x100)){
  try{
   auto current=std::atomic_load_explicit(&owner,std::memory_order_acquire);
   uintptr_t saved{},bundle{};
   if(current && uintptr_t(presenter)==current->presenter && Live(*current) &&
      Value(slot,saved) && saved==caller && Value(slot+0xf8,bundle)){
    std::array<unsigned char,0xc1> data{};std::array<int64_t,2> summary{};
    if(Read(bundle,data.data(),data.size()) && Read(slot+0x48,summary.data(),sizeof(summary))){
     uintptr_t resources{},outputResource{};uint64_t count{},bundleFrame{};UINT bundleSync{},bundleFlags{};
     memcpy(&resources,data.data(),8);memcpy(&count,data.data()+8,8);
     memcpy(&bundleSync,data.data()+0x28,4);memcpy(&bundleFlags,data.data()+0x2c,4);memcpy(&bundleFrame,data.data()+0x30,8);
     const auto position=uintptr_t(resource);
     if(count>=2 && count<=3 && resources && resources<=UINTPTR_MAX-24 &&
        position>resources && position-resources<count*8 && !((position-resources)&7) &&
        bundleSync==sync && bundleFlags==flags && bundleFrame==frame &&
        uintptr_t(parameters)==bundle+0x38 && Value(position,outputResource) && outputResource &&
        summary[0]>=1 && summary[0]<=9 && summary[1]>=1000 && summary[1]<=250000000 &&
        !current->retired.load(std::memory_order_acquire))
      try {nativePace(presenter,(const void*)bundle,uint8_t(data[0xc0]&1),(const void*)(slot+0x48),uint32_t((position-resources)/8));}
      catch(...){current->retired.store(true,std::memory_order_release);Log("XeSS output pacing: SDK wait raised an exception; compatibility owner disabled, original Present retained");}
    }
   }
  }catch(...){}
 }
 SetLastError(error);return originalPresent(presenter,sync,flags,frame,parameters,resource,generated);
}
}
bool Install(HMODULE module) noexcept {
 try {
  std::lock_guard<std::mutex> lock(installMutex);
  if(providerModule)return providerModule==module;
  const auto& layout=xess_layout::Get(module);
  // The private offsets below are accepted only for this witnessed SDK family.
  layout.VerifyFunction(module,0x224b30);layout.VerifyFunction(module,0x21ee30);layout.VerifyFunction(module,0x21f730);
  returnSite=uintptr_t(module)+layout.Rva(0x21ef96);
  implementationTable=uintptr_t(module)+layout.Rva(0x157c3a0);
  chainTable=uintptr_t(module)+layout.Rva(0x157a980);getter=uintptr_t(module)+layout.Rva(0xf410);
  original=reinterpret_cast<Deadline>(uintptr_t(module)+layout.Rva(0x224b30));
  originalPresent=reinterpret_cast<Present>(uintptr_t(module)+layout.Rva(0x21f730));
  nativePace=reinterpret_cast<Pace>(uintptr_t(module)+layout.Rva(0x21ee30));
  extraReturnSite=uintptr_t(module)+layout.Rva(0x2202ed);
  // Installed before creating our first SDK context. Keep callbacks and SDK
  // mapped for process lifetime, even across context destruction/recreation.
  HMODULE pinned{};
  if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
      reinterpret_cast<LPCWSTR>(&Hook),&pinned))return false;
  const LONG begin=DetourTransactionBegin();if(begin!=NO_ERROR)return false;
  LONG result=DetourUpdateThread(GetCurrentThread());
  if(result==NO_ERROR)result=DetourAttach(reinterpret_cast<void**>(&original),reinterpret_cast<void*>(&Hook));
  if(result==NO_ERROR)result=DetourAttach(reinterpret_cast<void**>(&originalPresent),reinterpret_cast<void*>(&HookPresent));
  if(result!=NO_ERROR){DetourTransactionAbort();Log("XeSS output pacing hook refused: %ld; SDK deadline retained",result);return false;}
  result=DetourTransactionCommit();
  if(result!=NO_ERROR){Log("XeSS output pacing hook transaction failed: %ld; SDK deadline retained",result);return false;}
  providerModule=module;Log("XeSS output pacing: verified deadlines and MFG intermediate waits installed; 2x/3x/4x, owner binding required");return true;
 }catch(const std::exception& e){Log("XeSS output pacing unavailable: %s; SDK deadline retained",e.what());return false;}
 catch(...){Log("XeSS output pacing unavailable; SDK deadline retained");return false;}
}
bool Bind(void* context,ID3D12Device* device) noexcept {
 try {
  if(!providerModule || !context || !device)return false;
  auto current=std::make_shared<Owner>();current->context=context;current->device=uintptr_t(device);
  if(!Value(uintptr_t(context)+8,current->implementation) ||
     !Value(current->implementation+8,current->chain) ||
     !Value(current->chain+0x120,current->presenter) ||
     !Value(current->chain+0x30,current->native) || !Live(*current)){
   Log("XeSS output pacing: context ownership/mode rejected; SDK deadline retained");return false;
  }
  auto previous=std::atomic_load_explicit(&owner,std::memory_order_acquire);
  if(previous && !previous->retired.load(std::memory_order_acquire))return false;
  std::atomic_store_explicit(&owner,current,std::memory_order_release);
  Log("XeSS output pacing: own software-pacer context bound; six-frame batch cadence enabled");return true;
 }catch(...){Log("XeSS output pacing: owner registration failed; SDK deadline retained");return false;}
}
void Reset(void* context) noexcept {
 try {auto current=std::atomic_load_explicit(&owner,std::memory_order_acquire);
  if(current && current->context==context){std::lock_guard<std::mutex> lock(current->mutex);current->clock.Reset();}
 }catch(...){}
}
void Unbind(void* context) noexcept {
 try {auto current=std::atomic_load_explicit(&owner,std::memory_order_acquire);
  if(!current || current->context!=context)return;
  current->retired.store(true,std::memory_order_release);
  std::atomic_store_explicit(&owner,std::shared_ptr<Owner>{},std::memory_order_release);
  std::lock_guard<std::mutex> lock(current->mutex);current->clock.Reset();
 }catch(...){}
}
}
