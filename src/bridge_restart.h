#pragma once
#include <atomic>
#include <cstdint>
namespace fgvk {
// UI only queues a complete request. It cannot consume/change the SDK while
// old resources are live; the WSI recreation owner supplies the retirement gate.
class BridgeRestart {
 std::atomic<uint32_t> pending_{};
public:
 bool Request(bool xess,unsigned preset){
  if(preset>3)return false;
  uint32_t empty=0;return pending_.compare_exchange_strong(empty,0x100u|(xess?1u:0u)|(preset<<2));
 }
 bool Pending()const{return pending_.load(std::memory_order_acquire)!=0;}
 bool Cancel(){return pending_.exchange(0,std::memory_order_acq_rel)!=0;}
 uint32_t Take(bool retired){return retired?pending_.exchange(0,std::memory_order_acq_rel):0;}
};
}
