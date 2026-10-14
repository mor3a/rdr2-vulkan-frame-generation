#pragma once
#include <cstdint>
namespace fgvk {
enum class PresentRoute { FrameGeneration, Native };
// A present naming only swapchains never created for the owned device belongs
// to another presenter (overlay, capture tool, auxiliary device). Streamline
// has no state for it, so it must reach the driver untouched instead of
// running frame-generation bookkeeping against a foreign queue.
inline PresentRoute RoutePresent(bool registryEmpty, bool anyKnown, uint32_t swapchainCount){
  if(!registryEmpty && swapchainCount>0 && !anyKnown) return PresentRoute::Native;
  return PresentRoute::FrameGeneration;
}
}
