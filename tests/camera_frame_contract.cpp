// Production camera code, CPU only. ScriptHook, rendering and the game are never called.
#include "../src/rdr_camera.cpp"
#include "camera_frame.h"
#include <cassert>
#include <cstdio>
namespace fgvk {
Config testConfig;
const Config& Cfg(){return testConfig;}
void Log(const char*,...){}
bool DebugLoggingEnabled(){return false;}
}
int main(){
 using namespace fgvk;
 sl::Constants oldA{},oldB{};
 assert(ApplyNeutralCamera(oldA,16.f/9,true,false));
 assert(ApplyNeutralCamera(oldB,16.f/9,true,false));
 assert(oldA.reset==sl::Boolean::eTrue && oldB.reset==sl::Boolean::eTrue); // bridge never committed this helper
 ResetFrameCameraHistory();
 for(uint64_t i=1;i<=500;++i){sl::Constants c{};assert(PrepareFrameCamera(i,c,16.f/9,true,false));assert(ResolveFrameCamera(i,c));assert((c.reset==sl::Boolean::eTrue)==(i==1));CommitFrameCamera(i,true);}
 // A discarded future SR pass must not become the camera of an earlier present.
 ResetFrameCameraHistory();testConfig.cameraSource=CameraSource::Sampled;
 const auto now=CameraNowUs();
 poseRing={};poseRing[0].position={0,0,0};poseRing[0].rotation={0,0,0};poseRing[0].fov=60;poseRing[0].valid=true;poseRing[0].timeUs=now-1000;
 CameraPose selected{};assert(ReadCameraPoseAt(now,0,50,selected) && selected.timeUs==now-1000);
 poseRing[1]=poseRing[0];poseRing[1].position={2,0,0};poseRing[1].timeUs=now+1000000;
 assert(ReadCameraPoseAt(now,0,50,selected) && selected.position[0]==0); // future pose rejected
 assert(!ReadCameraPoseAt(now+100000,0,50,selected)); // stale past + future candidates rejected
 sl::Constants a{},b{};assert(PrepareFrameCamera(1,a,16.f/9,true,false));
 poseRing[1].timeUs=CameraNowUs()-500;
 assert(PrepareFrameCamera(2,b,16.f/9,true,false));
 assert(ResolveFrameCamera(1,a) && a.cameraPos.x==0);CommitFrameCamera(1,true);
 assert(ResolveFrameCamera(2,b) && b.cameraPos.x==2 && b.reset==sl::Boolean::eFalse);CommitFrameCamera(2,true);
 // Missing sampling falls back to finite neutral constants, not an FG-off decision.
 poseRing={};sl::Constants fallback{};assert(PrepareFrameCamera(3,fallback,16.f/9,true,false));assert(ResolveFrameCamera(3,fallback));
 assert(ReadFrameCameraInfo().actual==CameraSource::Neutral);CommitFrameCamera(3,true);
 // Dropped inputs cannot exhaust the finite camera ledger.
 for(uint64_t i=4;i<1000;++i){sl::Constants c{};assert(PrepareFrameCamera(i,c,16.f/9,true,false));CommitFrameCamera(i,false);}
 sl::Constants final{};assert(PrepareFrameCamera(1000,final,16.f/9,true,false));assert(ResolveFrameCamera(1000,final));CommitFrameCamera(1000,true);
 assert(FrameBudgetUs(0,120,3)==25000 && FrameBudgetUs(0,120,1)==8334);
 assert(FrameBudgetUs(30,120,3)==33334 && !FrameBudgetUs(0,0,6));
 puts("Camera production contract: reproduced legacy repeated reset; 500 neutral frames reset once; timestamp/future/stale selection; pipelined poses; successful-present history; missing-pose fallback; 996 discarded inputs; output budgets: PASS");
}
