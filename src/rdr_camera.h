#pragma once
#include <windows.h>
#include <sl.h>
namespace fgvk {
struct CameraPose {float position[3]{},rotation[3]{},fov{};uint64_t timeUs{};bool valid{};};
uint64_t CameraNowUs();
bool ReadCameraPoseAt(uint64_t target,uint64_t previous,uint32_t maxAgeMs,CameraPose& pose);
bool RegisterRdrCamera(HMODULE self);
// Game native calls only run on the ScriptHook fiber. The rendering thread
// reads a copied snapshot and never invokes natives itself.
void CommitRdrCamera(bool accepted);
void InvalidateRdrCameraHistory();
bool ApplyRdrCamera(sl::Constants& constants,float aspect,bool invertedDepth,bool reset,bool historyProtection=true,bool requireFreshPose=true);
bool ApplyNeutralCamera(sl::Constants& constants,float aspect,bool invertedDepth,bool reset,bool historyProtection=true);
}
