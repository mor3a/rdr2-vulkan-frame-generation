#pragma once
#include <atomic>
#include <cstdint>
namespace fgvk {
class DlssActivity {
 std::atomic<uint64_t> last{0};
public:
 void Completed(uint64_t now,bool success){if(success)last.store(now+1,std::memory_order_release);}
 uint64_t Age(uint64_t now)const{auto stamp=last.load(std::memory_order_acquire);return stamp && now>=stamp-1?now-(stamp-1):UINT64_MAX;}
 bool Recent(uint64_t now,uint32_t grace)const{return Age(now)<=grace;}
 void Reset(){last.store(0,std::memory_order_release);}
};
}
