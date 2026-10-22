#include "native_hud_history.h"
#include <cassert>
#include <cstdio>
int main(){
  using namespace fgvk;NativeHudHistory h;h.Add(11);h.Add(12);
  assert(!h.Record(1,11)); // no acquire, no guessed frame
  for(unsigned frame=1;frame<=1000;++frame){
    const uint64_t image=frame%2?11:12;
    h.Acquire(image);assert(!h.Matches(image,frame));
    assert(h.Record(1,image));assert(!h.Record(2,image)); // one snapshot only
    h.Forget(1);assert(h.Record(2,image)); // discarded recording can be replaced
    h.Execute(3,{2});h.Submit(3,frame);
    assert(h.Matches(image,frame));assert(!h.Matches(image,0));assert(!h.Matches(image,frame+1));
    h.Forget(2);h.Forget(3);assert(h.Matches(image,frame)); // submitted GPU work persists
    h.Acquire(image);assert(!h.Matches(image,frame)); // image reuse cannot inherit old HUD
  }
  h.Acquire(11);assert(h.Record(9,11));h.Acquire(11);h.Submit(9,1001);assert(!h.Matches(11,1001));
  h.Forget(9);assert(h.Record(9,11));h.Remove(11);h.Add(11);h.Acquire(11);h.Submit(9,1002);assert(!h.Matches(11,1002));
  assert(h.Record(9,11));h.Submit(9,0);assert(!h.Matches(11,1));
  puts("Native HUD history: 1000 image cycles; exact submission token; no stale, discarded, unacquired or recycled-image fallback; secondary command tracking: PASS");
}
