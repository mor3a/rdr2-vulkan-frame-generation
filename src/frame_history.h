#pragma once
#include <cstdint>

namespace fgvk {
// Owned by the frame queue mutex. Invalidation is acknowledged only by a
// successful present using the same snapshot, never by a later CPU evaluation.
class FrameHistory {
public:
  enum Reason : uint32_t { First=1, QueueDrop=2, Association=4, Missing=8,
                          Rejected=16, PresentFailure=32, IndexMismatch=64, Gap=128 };
  struct Decision { uint32_t index=0, reasons=0; uint64_t epoch=0; bool valid=false;
    constexpr bool Reset() const { return reasons!=0; } };
  constexpr void Invalidate(uint32_t reason) { pending_|=reason; ++epoch_; }
  constexpr Decision Select(uint32_t submitted,uint32_t slot,uint32_t sdk,
                            uint32_t tags,uint32_t constants) const {
    const bool valid=slot && slot>last_ && submitted==slot && sdk==slot && tags==slot && constants==slot;
    uint32_t reasons=pending_;
    if(!last_) reasons|=First;
    else if(slot!=last_+1) reasons|=Gap;
    if(!valid) reasons|=IndexMismatch;
    return {slot,reasons,epoch_,valid};
  }
  constexpr void Complete(const Decision& d,bool constantsAccepted,bool presented) {
    if(!d.valid || !constantsAccepted || !presented) {
      Invalidate(!presented?PresentFailure:Rejected); return;
    }
    last_=d.index;
    // A dropped/rejected future input can invalidate history while this frame
    // is inside vkQueuePresent. Do not consume that newer reset request.
    if(d.epoch==epoch_) pending_=0;
  }
  constexpr uint32_t Last() const { return last_; }
private:
  uint32_t last_=0,pending_=0;
  uint64_t epoch_=0;
};
}
