#include "bridge_restart.h"
#include "bridge_recovery_evidence.h"
#include "bridge_resume_history.h"
#include "bridge_lifecycle.h"
#include "fps_meter.h"
#include <thread>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace fgvk;
int main(){
 unsigned requests=0;
 for(unsigned round=0;round<1000;++round){
  BridgeRestart r;assert(!r.Request(false,4));assert(!r.Pending());
  std::array<std::thread,4> writers;
  std::atomic<unsigned> won{};
  for(unsigned i=0;i<4;++i)writers[i]=std::thread([&,i]{if(r.Request((i&1)!=0,i))++won;});
  for(auto& w:writers)w.join();assert(won==1 && r.Pending());
  assert(!r.Take(false) && r.Pending()); // retirement failure never switches backend
  auto request=r.Take(true);assert(request && !r.Pending() && !r.Take(true));
  auto preset=(request>>2)&3;assert((request&1)==(preset&1)); // no torn provider/preset pair
  ++requests;
 }
 // SDK reports queued batches asynchronously. Sampling must integrate the
 // REAL counts rather than reset whenever a poll returns zero or one.
 FPSMeter m;uint64_t now=100;m.Present(now);bool active=false;
 for(unsigned frame=0;frame<500;++frame){
  const bool next=BridgeCountingActive(true,0);
  if(next!=active){m.ResetDisplayed();active=next;}
  m.AddDisplayed(frame%2?4:0);now+=20;m.Present(now);
 }
 assert(m.outputValid && std::abs(m.realFPS-50)<.01f && std::abs(m.outputFPS-100)<.01f);
 for(unsigned frame=0;frame<50;++frame){m.AddDisplayed(1);now+=20;m.Present(now);}
 assert(std::abs(m.outputFPS-50)<.01f); // native-only output is NEVER fabricated as 2x
 assert(!BridgeCountingActive(false,0) && !BridgeCountingActive(true,1));
 xess_bridge::InputDimensions presented;assert(presented.Changed(1280,720));presented.Accept(1280,720);
 // A prepared new-size input may be discarded. Only actual successful
 // presentation advances this history, so the next new-size input still resets.
 assert(presented.Changed(960,540));assert(presented.Changed(960,540));presented.Accept(960,540);assert(!presented.Changed(960,540));
 puts("FG recovery: 1000 concurrent restart requests gated by retirement; actual asynchronous FPS batches/native-only counts; first PRESENTED new-size history: PASS");
 assert(requests==1000);
 BridgeRestart cancelled;assert(cancelled.Request(true,2));assert(cancelled.Cancel());
 assert(!cancelled.Pending() && !cancelled.Take(true) && !cancelled.Cancel());
 BridgeRecoveryEvidence evidence;evidence.Observe(true,true,false,true,99);assert(!evidence.presents);
 evidence.Begin(100,0,UINT32_MAX-1);
 for(unsigned i=0;i<250;++i)evidence.Observe(true,true,i==0,true,i%2?4:0);
 assert(!evidence.Ready(5099) && evidence.Ready(5100));
 assert(evidence.presents==250 && evidence.sdkOutputs==500 && evidence.multiQueries==125 && evidence.resets==1);
 assert(evidence.NativeDelta(0,498)==500);assert(!evidence.NativeDelta(-1,498));
 evidence.done=true;evidence.Observe(true,true,true,true,10);assert(evidence.sdkOutputs==500 && !evidence.Ready(6000));
 BridgeRecoveryEvidence unavailable;unavailable.Begin(100,-1,0);unavailable.Observe(false,false,false,false,999);
 assert(!unavailable.NativeDelta(0,100) && !unavailable.sdkOutputs && !unavailable.queries);
 puts("Bounded recovery evidence: actual asynchronous SDK counts, failed-query exclusion, native DXGI UINT wrap/unavailable counter, one-window limit: PASS");
 BridgeResumeHistory history;assert(history.NeedsReset(1));history.Presented(1,true,true);
 for(unsigned i=2;i<1000;++i){assert(!history.NeedsReset(i));history.Presented(i,true,true);}
 history.Presented(1000,false,true);assert(history.NeedsReset(1001));history.Presented(1001,true,true);assert(!history.NeedsReset(1002));
 history.Presented(1002,true,false);assert(history.NeedsReset(1003));history.Presented(1003,true,true);
 assert(history.NeedsReset(1005));history.Presented(1005,true,true);assert(!history.NeedsReset(1006));
 history.Invalidate();assert(history.NeedsReset(1006));history.Presented(1006,true,true);assert(!history.NeedsReset(1007));
 puts("Production resume history: 999 continuous frames, native input gap, failed/occluded present, missing epoch and explicit invalidation; one accepted reset frame then normal generation: PASS");
}
