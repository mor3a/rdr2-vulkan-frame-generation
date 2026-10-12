#pragma once
#include <windows.h>
#include <sl.h>
namespace fgvk {
bool RegisterRdrCamera(HMODULE self);
// Game native calls only run on the ScriptHook fiber. The rendering thread
// reads a copied snapshot and never invokes natives itself.
bool ApplyRdrCamera(sl::Constants& constants,float aspect,bool invertedDepth,bool reset);
}
