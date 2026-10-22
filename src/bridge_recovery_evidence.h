#pragma once
#include <cstdint>
#include <optional>
namespace fgvk {
// One bounded observation window per presentation generation, not a permanent
// high-frequency logger. DXGI native presents are submissions, not scanout.
struct BridgeRecoveryEvidence {
 bool started{},done{};uint64_t startMs{},presents{},eligible{},hud{},resets{},queries{},sdkOutputs{},multiQueries{};
 int64_t nativeStartResult{};uint32_t nativeStart{};
 void Begin(uint64_t now,int64_t result,uint32_t count){started=true;startMs=now;nativeStartResult=result;nativeStart=count;}
 void Observe(bool requested,bool hasHud,bool reset,bool valid,uint32_t outputs){
  if(!started || done)return;
  ++presents;eligible+=requested;hud+=hasHud;resets+=reset;
  if(valid){++queries;sdkOutputs+=outputs;multiQueries+=outputs>1;}
 }
 bool Ready(uint64_t now)const{return started && !done && now>=startMs && now-startMs>=5000;}
 std::optional<uint32_t> NativeDelta(int64_t endResult,uint32_t end)const{
  if(!started || nativeStartResult<0 || endResult<0)return {};
  return uint32_t(end-nativeStart); // DXGI's UINT counter can wrap.
 }
};
}
