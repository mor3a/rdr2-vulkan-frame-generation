#pragma once
#include <cstdint>
namespace fgvk {
enum class FGState { Off, Waiting, EnabledUnmeasured, Generating, Error };
// Values follow the pinned Streamline sl_result.h. A warning is not a failure.
constexpr bool FGResultAccepted(int r) { return r==0 || r==39; }
struct FGSnapshot {
  FGState state{FGState::Off}; bool requested{},accepted{};
  int optionsResult{},queryResult{}; uint32_t lastPresented{},status{};
  uint64_t optionCalls{},polls{},querySuccess{},queryWarnings{},queryErrors{};
  float measuredRatio{}; bool dlssgSupported{true},dlssgSupportKnown{};
};
inline FGState ClassifyFG(bool requested,bool accepted,bool failed,bool measured,float ratio) {
  if(failed)return FGState::Error;
  if(!requested)return FGState::Off;
  if(!accepted)return FGState::Waiting;
  return measured && ratio>1.01f ? FGState::Generating : FGState::EnabledUnmeasured;
}
FGSnapshot GetFGSnapshot();
}
