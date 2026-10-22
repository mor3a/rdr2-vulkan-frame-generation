#pragma once
#include <cstdint>
namespace fgvk {
struct FGPresetSnapshot { int result=-1; uint32_t preset=0;uint64_t feedback=0; bool reported=false; int profileResult=-1; uint32_t profilePreset=0; char dllVersion[40]{}; bool providerRead=false; uint32_t providerPreset=0,presetRequested=0,presetError=0; uint64_t presetReads=0; };
void StartFGPresetMonitor();
FGPresetSnapshot GetFGPresetSnapshot();
FGPresetSnapshot QueryFGPreset(uint32_t processId);
const char* FGPresetName(uint32_t value,char* buffer,unsigned capacity);
}
