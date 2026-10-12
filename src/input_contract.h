#pragma once
#include <cmath>
#include <cstdint>
namespace fgvk {
inline bool ValidExtent(uint32_t w,uint32_t h){ return w>0 && h>0 && w<=16384 && h<=16384; }
inline bool ValidScalars(float jx,float jy,float sx,float sy){
  return std::isfinite(jx) && std::isfinite(jy) && std::isfinite(sx) && std::isfinite(sy)
      && std::abs(jx)<=8.f && std::abs(jy)<=8.f && sx!=0.f && sy!=0.f;
}
}
