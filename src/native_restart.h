#pragma once
#include <atomic>
#include <cstdint>
namespace fgvk {
// The menu publishes a request. Only the present owner advances its stages.
// Resources are never released until an Off-mode present has completed.
class NativeRestartState {
  std::atomic<uint32_t> request_{};
  std::atomic<bool> failed_{};
public:
  enum class Action {None,Disable,Rebuild};
  bool Request(unsigned preset){
    if(preset>3)return false;
    uint32_t empty=0;
    return request_.compare_exchange_strong(empty,0x100u|preset);
  }
  bool Pending()const{return request_.load(std::memory_order_acquire)!=0;}
  bool Failed()const{return failed_.load(std::memory_order_acquire);}
  unsigned Preset()const{return request_.load(std::memory_order_acquire)&3u;}
  Action Next()const{
    const auto stage=request_.load(std::memory_order_acquire)&0xf00u;
    return stage==0x100?Action::Disable:stage==0x300?Action::Rebuild:Action::None;
  }
  void Disabled(bool ok){
    if(!ok){Complete(false);return;}
    auto value=request_.load();
    if((value&0xf00u)==0x100u)request_.store(0x200u|(value&3u),std::memory_order_release);
  }
  void Presented(bool ok){
    if(!ok)return;
    auto value=request_.load();
    if((value&0xf00u)==0x200u)request_.store(0x300u|(value&3u),std::memory_order_release);
  }
  void Complete(bool ok){failed_.store(!ok,std::memory_order_release);request_.store(0,std::memory_order_release);}
};
enum class NativeInputReset {Busy,Complete,Failed};
void RequestNativeRestart(unsigned preset);
bool NativeRestartPending();
bool NativeRestartFailed();
NativeInputReset ResetNativeFGInputs(unsigned preset);
}
