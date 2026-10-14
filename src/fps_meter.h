#pragma once
#include <cstdint>
namespace fgvk {
struct FPSMeter {
  uint64_t start{},lastPresent{},rendered{},displayed{},readings{};uint32_t staleBuckets{};
  float realFPS{},outputFPS{};bool outputValid{};
  void AddDisplayed(uint32_t n){displayed+=n;++readings;}
  bool Present(uint64_t now){
    if(!lastPresent || now-lastPresent>2000){start=now;rendered=0;displayed=0;readings=0;staleBuckets=0;outputValid=false;lastPresent=now;return false;}
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
