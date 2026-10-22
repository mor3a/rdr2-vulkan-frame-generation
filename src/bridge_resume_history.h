#pragma once
#include <cstdint>
namespace fgvk {
// Presentation/input discontinuity, never camera speed. One successfully
// accepted reference frame consumes the reset; continuous frames do not reset.
struct BridgeResumeHistory {
 bool pending=true;uint64_t last{};
 void Invalidate(){pending=true;}
 bool NeedsReset(uint64_t epoch)const{return pending || !last || epoch!=last+1;}
 void Presented(uint64_t epoch,bool generated,bool accepted){
  if(generated && accepted){last=epoch;pending=false;}else pending=true;
 }
};
}
