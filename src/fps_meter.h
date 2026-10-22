#pragma once
#include <cstdint>
namespace fgvk {
// The SDK reports completed batches asynchronously, including 0/1 counts
// between batches. Count those reports without erasing the sampling window.
constexpr bool BridgeCountingActive(bool requested,uint32_t sdkStatus){return requested && sdkStatus==0;}
struct FPSMeter {
  uint64_t start{},lastPresent{},rendered{},displayed{},readings{};uint32_t staleBuckets{};
  float realFPS{},outputFPS{};bool outputValid{};
  void ResetDisplayed(){displayed=readings=staleBuckets=0;outputFPS=0;outputValid=false;}
  void AddDisplayed(uint32_t n){displayed+=n;++readings;}
  bool Present(uint64_t now){
    if(!lastPresent){start=now;rendered=0;displayed=0;readings=0;staleBuckets=0;realFPS=0;outputFPS=0;outputValid=false;lastPresent=now;return false;}
    // A long gap means the previous window is meaningless. Publish the cleared
    // values once (return true) so the counter cannot keep showing a stale FPS
    // indefinitely while the game is paused or not presenting.
    if(now-lastPresent>2000){start=now;rendered=0;displayed=0;readings=0;staleBuckets=0;realFPS=0;outputFPS=0;outputValid=false;lastPresent=now;return true;}
    lastPresent=now;++rendered;
    if(now-start<1000)return false;
    float seconds=float(now-start)/1000.f;
    realFPS=float(rendered)/seconds;float measured=float(displayed)/seconds;
    // Never invent generated FPS from the selected multiplier when telemetry drops.
    // A brief dropout (a failed state query or a zero presented count for one
    // sampling window) keeps the last measured value for up to two windows
    // instead of flashing "--" while generation is visibly running.
    if(readings>=rendered*9/10 && readings>0 && displayed>0){outputFPS=measured;outputValid=true;staleBuckets=0;}
    else if(outputValid && ++staleBuckets<=2){}
    else outputValid=false;
    start=now;rendered=displayed=readings=0;return true;
  }
};
}
