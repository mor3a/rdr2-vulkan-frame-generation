#pragma once
#include <cstdint>
namespace fgvk {
struct FPSMeter {
  uint64_t start{},lastPresent{},rendered{},displayed{},readings{};
  float realFPS{},outputFPS{};bool outputValid{};
  void AddDisplayed(uint32_t n){displayed+=n;++readings;}
  bool Present(uint64_t now){
    if(!lastPresent || now-lastPresent>2000){start=now;rendered=0;displayed=0;readings=0;lastPresent=now;return false;}
    lastPresent=now;++rendered;
    if(now-start<1000)return false;
    float seconds=float(now-start)/1000.f;
    realFPS=float(rendered)/seconds;outputFPS=float(displayed)/seconds;
    // Never invent generated FPS from the selected multiplier when telemetry drops.
    outputValid=readings>=rendered*9/10 && readings>0 && displayed>0;
    start=now;rendered=displayed=readings=0;return true;
  }
};
}
