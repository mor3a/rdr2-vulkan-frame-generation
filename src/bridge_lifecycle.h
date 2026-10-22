#pragma once
#include <cstdint>
namespace fgvk::xess_bridge {
template<class Surface> constexpr bool ReplacePresentation(Surface current,Surface requested,bool noOld,bool ownedOld){
  // Some games recreate WSI without passing oldSwapchain. Only replace our
  // current presentation for its own surface; never hijack a second window.
  return current==requested && (noOld || ownedOld);
}
constexpr bool InputRegionFits(uint32_t width,uint32_t height,int32_t x,int32_t y,uint32_t activeW,uint32_t activeH){
  return x>=0 && y>=0 && activeW && activeH && uint64_t(x)+activeW<=width && uint64_t(y)+activeH<=height;
}
struct InputDimensions {
  uint32_t width{},height{};
  bool Changed(uint32_t w,uint32_t h)const{return width!=w || height!=h;}
  bool Accept(uint32_t w,uint32_t h){
    const bool changed=width!=w || height!=h;
    width=w;height=h;return changed;
  }
};
}
