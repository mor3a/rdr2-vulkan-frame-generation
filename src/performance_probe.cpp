#include "performance_probe.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <vector>
#include <algorithm>
namespace fgvk {
static int phase=-1;static ULONGLONG begin{},nextPoll{};
static ULONGLONG lastWorld{};
static bool savedOff{},savedPacing{};static uint32_t savedFrames{};
static uint64_t frames{},shown{};static double pacing{},present{},markers{};
static std::vector<double> intervals;static LARGE_INTEGER lastCounter{},frequency{};
static const char* names[]={"OFF-A","2x-PACED-A","2x-UNPACED","3x-PACED","3x-UNPACED","2x-PACED-B","OFF-B"};
static bool collecting(){return phase>=0 && GetTickCount64()-begin>=6000;}
static void restore(){Rt().fgUserOff=savedOff;Rt().presentPacing=savedPacing;Rt().frames=savedFrames;phase=-1;EndPerformanceProbe();Log("PERF: finished; original runtime settings restored");}
bool PerformanceProbeBegin(bool world){
  if(phase<0 && !PerformanceProbeRequested())return false;
  auto now=GetTickCount64();
  if(world)lastWorld=now;
  if(phase<0){
    if(now<nextPoll)return false;nextPoll=now+1000;
    if(!world||!PerformanceProbeRequested())return false;
    savedOff=Rt().fgUserOff;savedFrames=Rt().frames;savedPacing=Rt().presentPacing;phase=0;begin=now;frames=shown=0;pacing=present=markers=0;intervals.clear();intervals.reserve(4096);QueryPerformanceFrequency(&frequency);lastCounter={};
    Log("PERF: fixed-scene 126-second test; 6s warmup + 12s sample per stage");
  }else if(!world && now-lastWorld>2000){Log("PERF: aborted because DLSS world rendering stopped");restore();return true;}
  else if(now-begin>=18000){
    double seconds=double(now-begin-6000)/1000.0;
    std::sort(intervals.begin(),intervals.end());double p95=intervals.empty()?0:intervals[(intervals.size()-1)*95/100];
    Log("PERF RESULT %s: seconds=%.3f realFPS=%.2f displayedFPS=%.2f ratio=%.3f pacing=%.3fms present=%.3fms markers=%.3fms p95RealFrame=%.3fms frames=%llu",names[phase],seconds,frames/seconds,shown/seconds,frames?double(shown)/frames:0,frames?pacing/frames:0,frames?present/frames:0,frames?markers/frames:0,p95,(unsigned long long)frames);
    if(++phase==7){restore();return true;}
    begin=now;frames=shown=0;pacing=present=markers=0;intervals.clear();lastCounter={};
  }else return false;
  Rt().frames=phase==3||phase==4?2:1;Rt().fgUserOff=phase==0||phase==6;Rt().presentPacing=phase==1||phase==3||phase==5;
  Log("PERF STAGE %s: FG=%d pacing=%d",names[phase],!Rt().fgUserOff,(int)Rt().presentPacing);return true;
}
void PerformanceProbeDisplayed(uint32_t count){if(collecting())shown+=count;}
void PerformanceProbeFrame(double pacingMs,double presentMs,double markerMs){if(collecting()){LARGE_INTEGER now{};QueryPerformanceCounter(&now);if(lastCounter.QuadPart && frequency.QuadPart)intervals.push_back(double(now.QuadPart-lastCounter.QuadPart)*1000.0/frequency.QuadPart);lastCounter=now;++frames;pacing+=pacingMs;present+=presentMs;markers+=markerMs;}}
}
