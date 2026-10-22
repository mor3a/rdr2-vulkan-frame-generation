#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
namespace fgvk {
enum class CameraSource {Neutral,Sampled,Derived};
inline CameraSource ParseCameraSource(const char* value){return !_stricmp(value,"Sampled")?CameraSource::Sampled:!_stricmp(value,"Derived")?CameraSource::Derived:CameraSource::Neutral;}
inline const char* CameraSourceName(CameraSource source){return source==CameraSource::Sampled?"Sampled":source==CameraSource::Derived?"Derived":"Neutral";}
// Application-frame budgets, not physical display frame guarantees.
inline uint32_t FrameBudgetUs(uint32_t baseCap,uint32_t outputCap,uint32_t outputs){
 const uint64_t base=baseCap?(1000000ull+baseCap-1)/baseCap:0;
 const uint64_t target=outputCap?(1000000ull*(std::max)(1u,outputs)+outputCap-1)/outputCap:0;
 return uint32_t((std::min)(uint64_t(UINT32_MAX),(std::max)(base,target)));
}
inline bool CameraTimeEligible(uint64_t pose,uint64_t target,uint64_t previous,uint32_t maxAgeMs){
 return pose && pose<=target && pose>=previous && target-pose<=uint64_t(maxAgeMs)*1000;
}
}
